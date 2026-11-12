#include "LocalRepCounting.h"
#include "ExercisePresets.h"
#include <math.h>
#include <stdio.h>

bool LocalRepCounting::begin(ExerciseType exercise, CountingMode mode, unsigned long now) {
  activeMode = mode;
  filteredMagnitude = 0;
  filterInitialised = false;
  if (!repCounter.beginPreset(now, ExercisePresets::pause(exercise))) return false;
  continuousCounter.beginPreset(GeneralConfig::GYRO_BIAS_RAD_S, now,
                                ExercisePresets::continuous(exercise));
  return true;
}

int LocalRepCounting::count() const {
  return activeMode == PAUSE_MODE ? repCounter.repetitionCount() : continuousCounter.repetitionCount();
}

void LocalRepCounting::reportRepResult(RepResult result, Log log) {
  if (!log || result == REP_NO_RESULT) {
    return;
  }

  const char* reason = "";
  switch (result) {
    case REP_TOO_SHORT: reason = "movement too short"; break;
    case REP_TOO_LONG: reason = "timeout before a completed rep with a pause"; break;
    case REP_TOO_WEAK:
      reason = repCounter.lastCompletedDuration() < repCounter.minimumRepDuration()
        ? "peak too weak AND movement too short"
        : "movement peak too weak";
      break;
    default: return;
  }

  char message[192];
  snprintf(message, sizeof(message),
    "REP skipped: %s | Peak: %.2f m/s2 / allowed >= %.2f m/s2 | Time: %.2f s / allowed %.2f-%.2f s",
    reason, repCounter.lastCompletedPeak(), repCounter.requiredRepPeak(),
    repCounter.lastCompletedDuration() / 1000.0f,
    repCounter.minimumRepDuration() / 1000.0f,
    repCounter.maximumRepDuration() / 1000.0f);
  log(message);
}

bool LocalRepCounting::update(float rawMagnitude, const float gyro[3], unsigned long now, Log log) {
  if (!filterInitialised) {
    filteredMagnitude = rawMagnitude;
    filterInitialised = true;
  } else {
    filteredMagnitude = GeneralConfig::FILTER_ALPHA * rawMagnitude +
      (1.0f - GeneralConfig::FILTER_ALPHA) * filteredMagnitude;
  }
  float correctedGyroSquared = 0.0f;

  for (int i = 0; i < 3; ++i) {
    const float corrected = gyro[i] - GeneralConfig::GYRO_BIAS_RAD_S[i];
    correctedGyroSquared += corrected * corrected;
  }

  const float correctedGyroMagnitude =
    sqrtf(correctedGyroSquared);

  const float accelerationMotion = fabsf(
    filteredMagnitude - repCounter.baselineMagnitude()
  );

  const bool stationary =
    accelerationMotion < genericSettings.calibrationEndGate &&
    correctedGyroMagnitude < ModeConfig::QUIET_GYRO_LIMIT;

  if (activeMode == PAUSE_MODE) {
    repCounter.update(filteredMagnitude, now);
    reportRepResult(repCounter.consumeRepResult(), log);
  }

  if (activeMode == CONTINUOUS_MODE) {
    continuousCounter.update(gyro, stationary, now);
  }

  const bool pauseRepEvent =
    repCounter.consumeRepEvent();

  const bool continuousRepEvent =
    continuousCounter.consumeRepEvent();

  const bool countedRep =
    (activeMode == PAUSE_MODE && pauseRepEvent) ||
    (activeMode == CONTINUOUS_MODE && continuousRepEvent);

  return countedRep;
}

