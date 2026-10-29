#include "GenericPeakDetector.h"
#include <math.h>

namespace {
float median3(const float values[3]) {
  const float a = values[0], b = values[1], c = values[2];
  return a < b ? (b < c ? b : (a < c ? c : a))
               : (a < c ? a : (b < c ? c : b));
}
}

void GenericPeakDetector::reset() {
  haveSample_ = haveTrigger_ = false;
  lastSampleMs_ = lastTriggerMs_ = 0;
  resetSignal();
}

void GenericPeakDetector::resetSignal() {
  filterCount_ = filterIndex_ = 0;
  armed_ = true;
  quiet_ = candidate_ = aboveGate_ = qualified_ = falling_ = false;
}

void GenericPeakDetector::disarm() {
  armed_ = false;
  quiet_ = candidate_ = aboveGate_ = qualified_ = falling_ = false;
}

bool GenericPeakDetector::update(const SensorSample& sample, GenericPeakEvent& event) {
  float accelerationSquared = 0, gyroSquared = 0;
  for (int axis = 0; axis < 3; ++axis) {
    if (!isfinite(sample.acceleration[axis]) || !isfinite(sample.gyro[axis])) {
      resetSignal();
      return false;
    }
    accelerationSquared += sample.acceleration[axis] * sample.acceleration[axis];
    gyroSquared += sample.gyro[axis] * sample.gyro[axis];
  }
  if (!isfinite(accelerationSquared) || !isfinite(gyroSquared)) {
    resetSignal();
    return false;
  }
  const uint32_t now = sample.timestampMs;
  if (haveSample_) {
    const uint32_t elapsed = now - lastSampleMs_;
    if (!elapsed || elapsed >= 0x80000000u) return false;
    if (elapsed > GenericPeakConfig::MAX_SAMPLE_GAP_MS) resetSignal();
  }
  haveSample_ = true;
  lastSampleMs_ = now;
  acceleration_[filterIndex_] = fabsf(sqrtf(accelerationSquared) - GeneralConfig::GRAVITY_MPS2);
  gyro_[filterIndex_] = sqrtf(gyroSquared);
  filterIndex_ = (filterIndex_ + 1) % 3;
  if (filterCount_ < 3) ++filterCount_;
  if (filterCount_ < 3) return false;

  // Reject isolated one-sample spikes before testing the duration and peak shape.
  const float acceleration = median3(acceleration_);
  const float gyro = median3(gyro_);
  const float score = fmaxf(acceleration / GenericPeakConfig::ACCELERATION_GATE_MPS2,
                            gyro / GenericPeakConfig::GYRO_GATE_RAD_S);
  if (!armed_) {
    if (score <= GenericPeakConfig::REARM_FRACTION) {
      if (!quiet_) { quiet_ = true; quietSince_ = now; }
      if (now - quietSince_ >= GenericPeakConfig::REARM_MS) {
        armed_ = true;
        quiet_ = false;
      }
    } else {
      quiet_ = false;
    }
    return false;
  }
  if (haveTrigger_ && now - lastTriggerMs_ < GenericPeakConfig::MIN_TRIGGER_GAP_MS) {
    // Ignore a whole excursion that starts during cooldown, not just its onset.
    if (score >= 1.0f) disarm();
    return false;
  }
  if (!candidate_) {
    if (score < 1.0f) return false;
    candidate_ = aboveGate_ = true;
    qualified_ = falling_ = false;
    candidateSince_ = aboveSince_ = now;
    peakScore_ = score;
    peak_.peakMs = now;
    peak_.accelerationMps2 = acceleration;
    peak_.gyroRadS = gyro;
  }
  if (now - candidateSince_ > GenericPeakConfig::MAX_CANDIDATE_MS) {
    disarm(); // Do not emit an old plateau peak once its history may be gone.
    return false;
  }
  if (score > peakScore_) {
    peakScore_ = score;
    peak_.peakMs = now;
    peak_.accelerationMps2 = acceleration;
    peak_.gyroRadS = gyro;
    falling_ = false;
  }
  if (score >= 1.0f) {
    if (!aboveGate_) { aboveGate_ = true; aboveSince_ = now; }
    if (now - aboveSince_ >= GenericPeakConfig::MIN_ACTIVE_MS) qualified_ = true;
  } else {
    aboveGate_ = false;
    if (!qualified_) { disarm(); return false; }
  }
  if (!qualified_) return false;
  if (score > peakScore_ * (1.0f - GenericPeakConfig::PEAK_DROP_FRACTION)) {
    falling_ = false;
    return false;
  }
  if (!falling_) { falling_ = true; fallingSince_ = now; }
  if (now - fallingSince_ < GenericPeakConfig::FALL_CONFIRM_MS) return false;

  peak_.detectedMs = now;
  event = peak_;
  haveTrigger_ = true;
  lastTriggerMs_ = now;
  disarm();
  return true;
}

