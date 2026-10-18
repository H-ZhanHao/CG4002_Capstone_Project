#include "RepCounter.h"

#include <math.h>

RepCounter::RepCounter(
  const ExerciseSettings& settings
) : settings_(settings) {
}

void RepCounter::begin(
  unsigned long currentTime
) {
  state_ = BASELINE_CALIBRATION;

  baselineStartTime_ = currentTime;
  baselineSum_ = 0.0;
  baselineSamples_ = 0;
  baselineMagnitude_ = GeneralConfig::GRAVITY_MPS2;
  motionSignal_ = 0.0;

  calibrationCount_ = 0;

  highThreshold_ = 0.0;
  lowThreshold_ = 0.0;
  requiredRepPeak_ = 0.0;

  minimumRepDuration_ = 0;
  maximumRepDuration_ = 0;

  movementActive_ = false;
  movementPeak_ = 0.0;

  movementStartTime_ = 0;
  quietStartTime_ = 0;
  lastMovementEndTime_ = 0;

  lastCompletedPeak_ = 0.0;
  lastCompletedDuration_ = 0;

  repetitionCount_ = 0;
  repEvent_ = false;
  calibrationEvent_ = false;
  calibrationResult_ = CAL_NO_RESULT;
  repResult_ = REP_NO_RESULT;
}

bool RepCounter::beginPreset(unsigned long currentTime, const PauseCalibration& preset) {
  begin(currentTime);
  if (!preset.valid) return false;
  // Supply the nominal baseline instead of measuring it from moving samples.
  state_ = MOVEMENT_CALIBRATION;
  // The first movement needs no inter-rep gap, even immediately after boot.
  lastMovementEndTime_ = currentTime - GeneralConfig::MIN_REP_GAP_MS;
  return loadPreset(preset);
}

bool RepCounter::loadPreset(const PauseCalibration& preset) {
  if (!restoreCalibration(preset)) return false;
  calibrationCount_ = 0; // No movements were used for calibration.
  return true;
}

PauseCalibration RepCounter::calibration() const {
  PauseCalibration result;
  result.valid = state_ == REP_COUNTING;
  result.highThreshold = highThreshold_;
  result.lowThreshold = lowThreshold_;
  result.requiredPeak = requiredRepPeak_;
  result.minDuration = minimumRepDuration_;
  result.maxDuration = maximumRepDuration_;
  return result;
}

bool RepCounter::restoreCalibration(const PauseCalibration& calibration) {
  // A measured or explicitly supplied baseline must precede movement limits.
  if (!calibration.valid || state_ != MOVEMENT_CALIBRATION) return false;
  highThreshold_ = calibration.highThreshold;
  lowThreshold_ = calibration.lowThreshold;
  requiredRepPeak_ = calibration.requiredPeak;
  minimumRepDuration_ = calibration.minDuration;
  maximumRepDuration_ = calibration.maxDuration;
  calibrationCount_ = GeneralConfig::REQUIRED_STABLE_REPS;
  repetitionCount_ = 0;
  repEvent_ = false;
  calibrationEvent_ = false;
  calibrationResult_ = CAL_NO_RESULT;
  repResult_ = REP_NO_RESULT;
  resetMovementCycle();
  state_ = REP_COUNTING;
  return true;
}

void RepCounter::update(
  float filteredMagnitude,
  unsigned long currentTime
) {
  if (state_ == BASELINE_CALIBRATION) {
    baselineSum_ += filteredMagnitude;
    baselineSamples_++;

    if (
      currentTime - baselineStartTime_ >=
      GeneralConfig::BASELINE_TIME_MS
    ) {
      baselineMagnitude_ =
        baselineSum_ / baselineSamples_;

      state_ = MOVEMENT_CALIBRATION;

      resetMovementCycle();
    }

    motionSignal_ = fabs(
      filteredMagnitude - baselineMagnitude_
    );

    return;
  }

  motionSignal_ = fabs(
    filteredMagnitude - baselineMagnitude_
  );

  if (state_ == MOVEMENT_CALIBRATION) {
    processMovement(
      settings_.calibrationStartGate,
      settings_.calibrationEndGate,
      currentTime
    );
  } else {
    processMovement(
      highThreshold_,
      lowThreshold_,
      currentTime
    );
  }
}

void RepCounter::resetMovementCycle() {
  movementActive_ = false;
  movementPeak_ = 0.0;
  movementStartTime_ = 0;
  quietStartTime_ = 0;
}

void RepCounter::processMovement(
  float startThreshold,
  float endThreshold,
  unsigned long currentTime
) {
  if (!movementActive_) {
    bool enoughTimePassed =
      currentTime - lastMovementEndTime_ >=
      GeneralConfig::MIN_REP_GAP_MS;

    if (
      motionSignal_ > startThreshold &&
      enoughTimePassed
    ) {
      movementActive_ = true;
      movementStartTime_ = currentTime;
      movementPeak_ = motionSignal_;
      quietStartTime_ = 0;
    }

    return;
  }

  if (motionSignal_ > movementPeak_) {
    movementPeak_ = motionSignal_;
  }

  unsigned long activeMaximumDuration;

  if (state_ == REP_COUNTING) {
    activeMaximumDuration =
      maximumRepDuration_;
  } else {
    activeMaximumDuration =
      GeneralConfig::
        CALIBRATION_MAX_DURATION_MS;
  }

  if (
    currentTime - movementStartTime_ >
    activeMaximumDuration
  ) {
    lastCompletedPeak_ = movementPeak_;
    lastCompletedDuration_ = currentTime - movementStartTime_;
    if (state_ == MOVEMENT_CALIBRATION) {
      calibrationResult_ = CAL_TOO_LONG;
    } else {
      repResult_ = REP_TOO_LONG;
    }
    resetMovementCycle();
    lastMovementEndTime_ = currentTime;
    return;
  }

  if (motionSignal_ < endThreshold) {
    if (quietStartTime_ == 0) {
      quietStartTime_ = currentTime;
    }

    if (
      currentTime - quietStartTime_ >=
      GeneralConfig::QUIET_TIME_MS
    ) {
      completeMovement(currentTime);
    }
   } else {
    quietStartTime_ = 0;
  }
}

void RepCounter::completeMovement(
  unsigned long currentTime
) {
  unsigned long completedDuration =
    currentTime - movementStartTime_;

  float completedPeak = movementPeak_;

  lastCompletedDuration_ =
    completedDuration;

  lastCompletedPeak_ =
    completedPeak;

  // The quiet period already separates this movement from the next one.
  // Count it toward the minimum gap instead of adding a blind interval
  // after completion. Timeouts still impose a gap from the timeout itself.
  lastMovementEndTime_ = quietStartTime_;

  resetMovementCycle();

  if (state_ == MOVEMENT_CALIBRATION) {
    if (
      completedDuration <
      GeneralConfig::
        CALIBRATION_MIN_DURATION_MS
    ) {
      calibrationResult_ = CAL_TOO_SHORT;
      return;
    }

    if (
      completedDuration >
      GeneralConfig::
        CALIBRATION_MAX_DURATION_MS
    ) {
      calibrationResult_ = CAL_TOO_LONG;
      return;
    }

    if (
      completedPeak <
      settings_.minimumCalibrationPeak
    ) {
      calibrationResult_ = CAL_TOO_WEAK;
      return;
    }

    addCalibrationCandidate(
      completedPeak,
      completedDuration
    );

    return;
  }

  if (
    completedPeak < requiredRepPeak_
  ) {
    repResult_ = REP_TOO_WEAK;
    return;
  }

  if (
    completedDuration <
    minimumRepDuration_
  ) {
    repResult_ = REP_TOO_SHORT;
    return;
  }

  if (
    completedDuration >
    maximumRepDuration_
  ) {
    repResult_ = REP_TOO_LONG;
    return;
  }

  repetitionCount_++;
  repEvent_ = true;
}

void RepCounter::addCalibrationCandidate(
  float peak,
  unsigned long duration
) {
  if (state_ != MOVEMENT_CALIBRATION) {
    return;
  }

  // Guard before writing so an invalid count cannot overrun either array.
  if (calibrationCount_ < 0 ||
      calibrationCount_ >= GeneralConfig::REQUIRED_STABLE_REPS) {
    calibrationCount_ = 0;
  }

  calibrationPeaks_[calibrationCount_] =
    peak;

  calibrationDurations_[calibrationCount_] =
    duration;

  calibrationCount_++;
  calibrationEvent_ = true;
  calibrationResult_ = CAL_ACCEPTED;

  if (
    calibrationCount_ ==
    GeneralConfig::REQUIRED_STABLE_REPS
  ) {
    evaluateCalibrationCandidates();
  }
}

bool RepCounter::movementsAreSimilar(
  int first,
  int second
) const {
  float averagePeak =
    (
      calibrationPeaks_[first] +
      calibrationPeaks_[second]
    ) / 2.0;

  float averageDuration =
    (
      calibrationDurations_[first] +
      calibrationDurations_[second]
    ) / 2.0;

  if (
    averagePeak <= 0.0 ||
    averageDuration <= 0.0
  ) {
    return false;
  }

  float relativePeakDifference =
    fabs(
      calibrationPeaks_[first] -
      calibrationPeaks_[second]
    ) / averagePeak;

  float relativeDurationDifference =
    fabs(
      (float)calibrationDurations_[first] -
      (float)calibrationDurations_[second]
    ) / averageDuration;

  return (
    relativePeakDifference <=
      settings_.peakSimilarityTolerance &&

    relativeDurationDifference <=
      settings_.durationSimilarityTolerance
  );
}

void RepCounter::evaluateCalibrationCandidates() {
  if (calibrationCount_ != GeneralConfig::REQUIRED_STABLE_REPS) {
    return;
  }

  bool allSimilar = true;
  for (int first = 0; first < calibrationCount_; ++first) {
    for (int second = first + 1; second < calibrationCount_; ++second) {
      if (!movementsAreSimilar(first, second)) {
        allSimilar = false;
      }
    }
  }

  if (allSimilar) {
    finishCalibration();
    calibrationResult_ = CAL_FINISHED;
    return;
  }

  calibrationResult_ = CAL_MISMATCH;
  // Reject the oldest candidate, retaining the more recent ones.
  // With two candidates, this keeps only the newest movement.
  for (int i = 1; i < calibrationCount_; ++i) {
    calibrationPeaks_[i - 1] = calibrationPeaks_[i];
    calibrationDurations_[i - 1] = calibrationDurations_[i];
  }
  --calibrationCount_;
}

void RepCounter::finishCalibration() {
  if (calibrationCount_ != GeneralConfig::REQUIRED_STABLE_REPS) {
    return;
  }

  float totalPeak = 0.0f;
  float totalDuration = 0.0f;
  for (int i = 0; i < calibrationCount_; ++i) {
    totalPeak += calibrationPeaks_[i];
    totalDuration += calibrationDurations_[i];
  }

  float averagePeak = totalPeak / calibrationCount_;
  float averageDuration = totalDuration / calibrationCount_;

  highThreshold_ =
    averagePeak *
    settings_.highThresholdFactor;

  lowThreshold_ =
    averagePeak *
    settings_.lowThresholdFactor;

  requiredRepPeak_ =
    averagePeak *
    settings_.requiredPeakFactor;

  if (highThreshold_ < 0.45) {
    highThreshold_ = 0.45;
  }

  if (lowThreshold_ < 0.20) {
    lowThreshold_ = 0.20;
  }

  if (
    requiredRepPeak_ <
    settings_.minimumCalibrationPeak
  ) {
    requiredRepPeak_ =
      settings_.minimumCalibrationPeak;
  }

  minimumRepDuration_ =
    (unsigned long)(
      averageDuration *
      settings_.minimumDurationFactor
    );

  maximumRepDuration_ =
    (unsigned long)(
      averageDuration *
      settings_.maximumDurationFactor
    );

  if (minimumRepDuration_ < 500) {
    minimumRepDuration_ = 500;
  }

  if (maximumRepDuration_ > 8000) {
    maximumRepDuration_ = 8000;
  }

  if (
    maximumRepDuration_ <=
    minimumRepDuration_
  ) {
    maximumRepDuration_ =
      minimumRepDuration_ + 500;
  }

  repetitionCount_ = 0;
  state_ = REP_COUNTING;

  resetMovementCycle();
}

bool RepCounter::consumeCalibrationEvent() {
  bool result = calibrationEvent_;
  calibrationEvent_ = false;
  return result;
}

CalibrationResult RepCounter::consumeCalibrationResult() {
  const CalibrationResult result = calibrationResult_;
  calibrationResult_ = CAL_NO_RESULT;
  return result;
}

bool RepCounter::consumeRepEvent() {
  bool result = repEvent_;
  repEvent_ = false;

  return result;
}

RepResult RepCounter::consumeRepResult() {
  const RepResult result = repResult_;
  repResult_ = REP_NO_RESULT;
  return result;
}

CounterState RepCounter::state() const {
  return state_;
}

float RepCounter::motionSignal() const {
  return motionSignal_;
}

float RepCounter::baselineMagnitude() const {
  return baselineMagnitude_;
}

float RepCounter::highThreshold() const {
  return highThreshold_;
}

float RepCounter::lowThreshold() const {
  return lowThreshold_;
}

float RepCounter::requiredRepPeak() const {
  return requiredRepPeak_;
}

float RepCounter::lastCompletedPeak() const {
  return lastCompletedPeak_;
}

unsigned long
RepCounter::minimumRepDuration() const {
  return minimumRepDuration_;
}

unsigned long
RepCounter::maximumRepDuration() const {
  return maximumRepDuration_;
}

unsigned long
RepCounter::lastCompletedDuration() const {
  return lastCompletedDuration_;
}

int RepCounter::repetitionCount() const {
  return repetitionCount_;
}

int RepCounter::calibrationCount() const {
  return calibrationCount_;
}
