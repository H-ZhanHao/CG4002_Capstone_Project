#pragma once

#include "ExerciseProfiles.h"

// Fixed starting values, not measured calibration profiles. Tune with the
// sensor worn on the assigned board. All continuous-mode values are mocks.
namespace ExercisePresets {
inline CountingMode defaultMode(ExerciseType exercise) {
  return exercise == JUMPING_JACK ? CONTINUOUS_MODE : PAUSE_MODE;
}

struct Preset {
  NodeRole role;
  const char* description;
  float start, end, peak; // Pause mode: acceleration magnitude deviation, m/s2.
  unsigned long minMs, maxMs; // Includes the finishing pause in pause mode.
  float gyroStart, gyroEnd, gyroPeak; // Continuous mode: rad/s.
  float halfAngle; // Minimum angular travel in EACH direction, radians.
  unsigned long gyroMinMs, gyroMaxMs;
};

inline const Preset& get(ExerciseType exercise) {
  // Order matches ExerciseType. Squat and lunge deliberately share limits.
  static const Preset presets[EXERCISE_COUNT] = {
    {HIP,   "provisional hip push-up", 0.50f, 0.20f, 0.70f, 700, 6000, 0.60f, 0.18f, 0.70f, 0.60f, 600, 6000},
    {WRIST, "mock wrist sit-up",       0.50f, 0.20f, 0.70f, 700, 6000, 0.60f, 0.18f, 0.70f, 0.60f, 600, 6000},
    {HIP,   "legacy squat gates",      0.80f, 0.30f, 1.00f, 700, 6000, 0.60f, 0.18f, 0.70f, 0.60f, 600, 6000},
    {HIP,   "squat gates for lunges",   0.80f, 0.30f, 1.00f, 700, 6000, 0.60f, 0.18f, 0.70f, 0.60f, 600, 6000},
    {HIP,   "mock hip leg raise",      0.50f, 0.20f, 0.70f, 700, 6000, 0.60f, 0.18f, 0.70f, 0.60f, 600, 6000},
    {WRIST, "mock wrist jumping jack", 0.50f, 0.20f, 0.70f, 700, 6000, 0.60f, 0.18f, 0.70f, 0.60f, 600, 6000}
  };
  return presets[exercise];
}

inline bool allowed(NodeRole role, ExerciseType exercise) {
  return exercise >= PUSHUP && exercise < EXERCISE_COUNT && get(exercise).role == role;
}

inline PauseCalibration pause(ExerciseType exercise) {
  const Preset& preset = get(exercise);
  PauseCalibration result;
  result.valid = true;
  result.highThreshold = preset.start;
  result.lowThreshold = preset.end;
  result.requiredPeak = preset.peak;
  result.minDuration = preset.minMs;
  result.maxDuration = preset.maxMs;
  return result;
}

inline ContinuousCalibration continuous(ExerciseType exercise) {
  const Preset& preset = get(exercise);
  ContinuousCalibration result;
  result.valid = true;
  // Axis and initial direction are discovered from movement each session.
  result.startGate = preset.gyroStart;
  result.endGate = preset.gyroEnd;
  result.requiredPeak = preset.gyroPeak;
  result.requiredAngle = preset.halfAngle;
  result.minDuration = preset.gyroMinMs;
  result.maxDuration = preset.gyroMaxMs;
  return result;
}
} // namespace ExercisePresets
