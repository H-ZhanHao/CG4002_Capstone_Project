#include "ExerciseRecognition.h"

namespace {
  // Set false when connecting the real recognition feature.
  constexpr bool USE_MOCK_RECOGNIZER = false;

  // This is a simulated recognition result, not an instruction
  // to the rep counter.
  constexpr ExerciseType MOCK_EXERCISE = SQUAT;

  // Measured from the start of MOVEMENT_CALIBRATION,
  // not from reset or baseline calibration.
  constexpr unsigned long MOCK_RESULT_DELAY_MS = 1500;
}

ExerciseRecognition::ExerciseRecognition()
  : active_(false),
    calibrationStartTime_(0),
    exercise_(UNKNOWN_EXERCISE) {
}

void ExerciseRecognition::beginCalibration(
  unsigned long currentTime
) {
  active_ = true;
  calibrationStartTime_ = currentTime;
  exercise_ = UNKNOWN_EXERCISE;
}

void ExerciseRecognition::update(
  const float acceleration[3],
  const float gyroscope[3],
  int calibrationCandidates,
  unsigned long currentTime
) {
  if (!active_ || exercise_ != UNKNOWN_EXERCISE) {
    return;
  }

  // The mock deliberately does not inspect these readings.
  // A real recognizer can consume or transmit these samples here:
  // acceleration: m/s²
  // gyroscope: rad/s
  (void)acceleration;
  (void)gyroscope;

  if (!USE_MOCK_RECOGNIZER) {
    // Future integration:
    // 1. Supply samples to the external recognition component.
    // 2. Receive its result without blocking the sensor loop.
    // 3. Pass a validated result to submitResult().
    //
    // With no result, exercise_ remains UNKNOWN.
    return;
  }

  // Require at least one accepted calibration candidate.
  // Simply standing still will not produce a mock result.
  if (calibrationCandidates < 1) {
    return;
  }

  if (
    currentTime - calibrationStartTime_ <
    MOCK_RESULT_DELAY_MS
  ) {
    return;
  }

  submitResult(MOCK_EXERCISE);
}

bool ExerciseRecognition::submitResult(
  ExerciseType exercise
) {
  if (!active_ || exercise_ != UNKNOWN_EXERCISE) {
    return false;
  }

  switch (exercise) {
    case SQUAT:
    case PUSH_UP:
    case SIT_UP:
    case JUMPING_JACK:
    case LUNGE:
    case LEG_RAISE:
      exercise_ = exercise;
      return true;

    case UNKNOWN_EXERCISE:
    default:
      return false;
  }
}

void ExerciseRecognition::endCalibration() {
  active_ = false;
}

ExerciseType ExerciseRecognition::exercise() const {
  return exercise_;
}

bool ExerciseRecognition::active() const {
  return active_;
}

bool ExerciseRecognition::usingMock() const {
  return USE_MOCK_RECOGNIZER;
}