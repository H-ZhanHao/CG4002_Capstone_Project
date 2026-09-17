#include "FallDetector.h"
#include "ExerciseConfig.h"

FallDetector::FallDetector() {
}

void FallDetector::begin(
  unsigned long currentTime
) {
  state_ = MONITORING_NORMAL;

  freeFallStartTime_ = currentTime;
  impactWindowStartTime_ = currentTime;

  fallEvent_ = false;
}

void FallDetector::update(
  float rawAccelerationMagnitude,
  unsigned long currentTime
) {
  switch (state_) {
    case MONITORING_NORMAL:
      if (
        rawAccelerationMagnitude <
        FallConfig::FREE_FALL_THRESHOLD
      ) {
        freeFallStartTime_ = currentTime;
        state_ = POSSIBLE_FREE_FALL;
      }
      break;

    case POSSIBLE_FREE_FALL:
      if (
        rawAccelerationMagnitude >=
        FallConfig::FREE_FALL_THRESHOLD
      ) {
        reset();
        break;
      }

      if (
        currentTime - freeFallStartTime_ >=
        FallConfig::FREE_FALL_MIN_TIME_MS
      ) {
        impactWindowStartTime_ = currentTime;
        state_ = WAITING_FOR_IMPACT;
      }
      break;

    case WAITING_FOR_IMPACT:
      if (
        rawAccelerationMagnitude >
        FallConfig::IMPACT_THRESHOLD
      ) {
        fallEvent_ = true;
        reset();
        break;
      }

      if (
        currentTime - impactWindowStartTime_ >
        FallConfig::IMPACT_WINDOW_MS
      ) {
        reset();
      }
      break;
  }
}

bool FallDetector::consumeFallEvent() {
  bool result = fallEvent_;
  fallEvent_ = false;

  return result;
}

FallDetectorState FallDetector::state() const {
  return state_;
}

void FallDetector::reset() {
  state_ = MONITORING_NORMAL;
}