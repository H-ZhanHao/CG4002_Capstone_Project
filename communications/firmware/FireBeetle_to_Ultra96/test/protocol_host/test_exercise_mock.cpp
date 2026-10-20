#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "ContinuousCounter.h"
#include "ExerciseConfig.h"
#include "ExercisePresets.h"
#include "RepCounter.h"

static void mockSample(unsigned index, float acceleration[3], float gyro[3]) {
  acceleration[0] = acceleration[1] = 0.0f;
  acceleration[2] = 9.80665f;
  gyro[0] = gyro[1] = gyro[2] = 0.0f;
  const unsigned cycle = index % 400U;
  if (cycle >= 260U && cycle < 320U) acceleration[2] += 1.2f;
  if (cycle >= 260U && cycle < 336U) gyro[0] = 2.0f;
  else if (cycle >= 336U && cycle < 366U) gyro[0] = -2.0f;
}

int main() {
  RepCounter pause(getGenericSettings());
  ContinuousCounter continuous;
  assert(pause.beginPreset(0, ExercisePresets::pause(SQUAT)));
  continuous.beginPreset(GeneralConfig::GYRO_BIAS_RAD_S, 0,
                         ExercisePresets::continuous(JUMPING_JACK));

  float filteredMagnitude = 0.0f;
  bool filterInitialised = false;
  for (unsigned index = 0; index < 400U; ++index) {
    float acceleration[3], gyro[3];
    mockSample(index, acceleration, gyro);
    const unsigned long now = index * 20U;
    const float magnitude = sqrtf(acceleration[0] * acceleration[0] +
      acceleration[1] * acceleration[1] + acceleration[2] * acceleration[2]);
    if (!filterInitialised) {
      filteredMagnitude = magnitude;
      filterInitialised = true;
    } else {
      filteredMagnitude = GeneralConfig::FILTER_ALPHA * magnitude +
        (1.0f - GeneralConfig::FILTER_ALPHA) * filteredMagnitude;
    }
    pause.update(filteredMagnitude, now);
    const float accelerationMotion = fabsf(filteredMagnitude - pause.baselineMagnitude());
    const bool stationary = accelerationMotion < getGenericSettings().calibrationEndGate &&
      fabsf(gyro[0]) < ModeConfig::QUIET_GYRO_LIMIT;
    continuous.update(gyro, stationary, now);
  }

  assert(pause.repetitionCount() == 1);
  assert(continuous.axis() == 0);
  assert(continuous.repetitionCount() == 1);
  puts("Mock waveform drives the original pause and continuous counters exactly once");
}
