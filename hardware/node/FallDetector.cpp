#include "FallDetector.h"
#include "ExerciseConfig.h"

#include <math.h>

namespace {
float magnitude(const float vector[3]) {
  return sqrtf(vector[0] * vector[0] + vector[1] * vector[1] +
               vector[2] * vector[2]);
}

bool changedPosture(const float before[3], const float after[3]) {
  const float dot = before[0] * after[0] + before[1] * after[1] +
                    before[2] * after[2];
  return dot <= FallConfig::POSTURE_CHANGE_COS * magnitude(before) * magnitude(after);
}

void filterGravity(float estimate[3], const float sample[3], float alpha) {
  for (int axis = 0; axis < 3; ++axis)
    estimate[axis] += alpha * (sample[axis] - estimate[axis]);
}
}

FallDetector::FallDetector() { begin(0); }

void FallDetector::begin(unsigned long currentTime) {
  state_ = MONITORING_NORMAL;
  freeFallStartTime_ = impactWindowStartTime_ = impactTime_ =
    lastSampleTime_ = quietStartTime_ = settledStartTime_ = (uint32_t)currentTime;
  for (int axis = 0; axis < 3; ++axis)
    gravity_[axis] = postImpactGravity_[axis] = 0.0f;
  haveSample_ = haveGravity_ = trackingQuiet_ = trackingSettled_ = false;
  fallEvent_ = false;
}

void FallDetector::update(const float acceleration[3], const float gyro[3],
                          unsigned long currentTime) {
  const uint32_t now = (uint32_t)currentTime;
  for (int axis = 0; axis < 3; ++axis) {
    if (!isfinite(acceleration[axis]) || !isfinite(gyro[axis])) {
      reset();
      haveSample_ = haveGravity_ = false;
      return;
    }
  }
  const uint32_t elapsed = now - lastSampleTime_;
  if (haveSample_ && elapsed == 0) return;
  if (haveSample_ && elapsed > FallConfig::MAX_SAMPLE_GAP_MS) {
    // Missing data cannot count as freefall or settled posture evidence.
    reset();
    haveGravity_ = false;
  }
  lastSampleTime_ = now;
  haveSample_ = true;

  const float rawMagnitude = magnitude(acceleration);
  float correctedGyro[3];
  for (int axis = 0; axis < 3; ++axis)
    correctedGyro[axis] = gyro[axis] - GeneralConfig::GYRO_BIAS_RAD_S[axis];
  const bool quiet = fabsf(rawMagnitude - GeneralConfig::GRAVITY_MPS2) <=
                      FallConfig::QUIET_ACCEL_TOLERANCE_MPS2 &&
                    magnitude(correctedGyro) <= FallConfig::QUIET_GYRO_RAD_S;
  const float alpha = elapsed / (FallConfig::GRAVITY_FILTER_TIME_MS + elapsed);

  switch (state_) {
    case MONITORING_NORMAL:
      // Learn gravity only near rest; freeze it throughout a fall candidate.
      // The direction is relative to the worn sensor, not a fixed mounting axis.
      if (quiet) {
        if (!trackingQuiet_) {
          trackingQuiet_ = true;
          quietStartTime_ = now;
          if (!haveGravity_)
            for (int axis = 0; axis < 3; ++axis) gravity_[axis] = acceleration[axis];
        }
        filterGravity(gravity_, acceleration, alpha);
        if (now - quietStartTime_ >= FallConfig::SETTLED_TIME_MS) haveGravity_ = true;
      } else {
        trackingQuiet_ = false;
      }
      if (haveGravity_ && rawMagnitude < FallConfig::FREE_FALL_THRESHOLD) {
        freeFallStartTime_ = now;
        state_ = POSSIBLE_FREE_FALL;
      }
      break;

    case POSSIBLE_FREE_FALL:
      if (rawMagnitude >= FallConfig::FREE_FALL_THRESHOLD) {
        reset();
      } else if (now - freeFallStartTime_ >= FallConfig::FREE_FALL_MIN_TIME_MS) {
        impactWindowStartTime_ = now;
        state_ = WAITING_FOR_IMPACT;
      }
      break;

    case WAITING_FOR_IMPACT:
      // Expire first so a late impact cannot resurrect an old candidate.
      if (now - impactWindowStartTime_ > FallConfig::IMPACT_WINDOW_MS) {
        reset();
      } else if (rawMagnitude > FallConfig::IMPACT_THRESHOLD) {
        impactTime_ = now;
        trackingQuiet_ = trackingSettled_ = false;
        state_ = CONFIRMING_FALL;
      }
      break;

    case CONFIRMING_FALL:
      if (quiet) {
        if (!trackingQuiet_) {
          for (int axis = 0; axis < 3; ++axis)
            postImpactGravity_[axis] = acceleration[axis];
          trackingQuiet_ = true;
        }
        filterGravity(postImpactGravity_, acceleration, alpha);
      } else {
        trackingQuiet_ = false;
      }
      // Check both the filtered and current direction so returning upright
      // cancels confirmation immediately despite filter lag.
      if (quiet && changedPosture(gravity_, postImpactGravity_) &&
          changedPosture(gravity_, acceleration)) {
        if (!trackingSettled_) {
          settledStartTime_ = now;
          trackingSettled_ = true;
        }
      } else {
        trackingSettled_ = false;
      }
      if (now - impactTime_ >= FallConfig::CONFIRMATION_TIME_MS) {
        if (trackingSettled_ && now - settledStartTime_ >= FallConfig::SETTLED_TIME_MS)
          fallEvent_ = true;
        reset();
      }
      break;
  }
}

bool FallDetector::consumeFallEvent() {
  const bool result = fallEvent_;
  fallEvent_ = false;
  return result;
}

FallDetectorState FallDetector::state() const { return state_; }

void FallDetector::reset() {
  state_ = MONITORING_NORMAL;
  trackingQuiet_ = trackingSettled_ = false;
}
