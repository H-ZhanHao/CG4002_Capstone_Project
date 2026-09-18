#include "ContinuousCounter.h"

#include <math.h>

void ContinuousCounter::begin(
  const float gyroBias[3],
  unsigned long now,
  bool requireContinuousCalibration
) {
  started_ = true;
  calibrated_ = false;

  requireContinuousCalibration_ =
    requireContinuousCalibration;

  calibrationCount_ = 0;
  repetitions_ = 0;

  repEvent_ = false;
  calibrationEvent_ = false;

  for (int i = 0; i < 3; ++i) {
    bias_[i] = gyroBias[i];
    filteredGyro_[i] = 0.0f;
    axisEnergy_[i] = 0.0f;
  }

  filterInitialised_ = false;
  axis_ = -1;
  axisSamples_ = 0;

  axisWindowStart_ = now;
  lastUpdate_ = now;

  signal_ = 0.0f;
  firstSign_ = 0.0f;

  phase_ = WAITING;
  endSettling_ = false;
  quietActive_ = false;

  cycleHadPause_ = false;
  previousCycleEnded_ = false;

  firstAngle_ = 0.0f;
  secondAngle_ = 0.0f;
  firstPeak_ = 0.0f;
  secondPeak_ = 0.0f;

  startGate_ = ContinuousConfig::INITIAL_START_GATE;
  endGate_ = ContinuousConfig::INITIAL_END_GATE;
  requiredAngle_ = ContinuousConfig::MIN_HALF_ANGLE;
  requiredPeak_ = 0.0f;

  minDuration_ = ContinuousConfig::MIN_CYCLE_TIME_MS;
  maxDuration_ = ContinuousConfig::MAX_CYCLE_TIME_MS;

  candidatePeak_ = 0.0f;
  candidateAngle_ = 0.0f;
  candidateDuration_ = 0;

  lastAngle_ = 0.0f;
  lastDuration_ = 0;
}

void ContinuousCounter::startCycle(unsigned long now) {
  phase_ = FIRST_DIRECTION;

  cycleStart_ = now;
  secondStart_ = now;

  firstAngle_ = 0.0f;
  secondAngle_ = 0.0f;

  firstPeak_ = 0.0f;
  secondPeak_ = 0.0f;

  endSettling_ = false;

  // In AUTO mode, long gaps between cycles indicate
  // that this is not consistently continuous movement.
  cycleHadPause_ =
    previousCycleEnded_ &&
    now - previousCycleEnd_ >= GeneralConfig::QUIET_TIME_MS;
}

void ContinuousCounter::update(
  const float gyro[3],
  bool stationary,
  unsigned long now
) {
  if (!started_) {
    return;
  }

  const unsigned long elapsed = now - lastUpdate_;
  lastUpdate_ = now;

  for (int i = 0; i < 3; ++i) {
    if (!isfinite(gyro[i])) {
      phase_ = WAITING;
      return;
    }

    const float corrected = gyro[i] - bias_[i];

    if (!filterInitialised_) {
      filteredGyro_[i] = corrected;
    } else {
      filteredGyro_[i] =
        ContinuousConfig::GYRO_FILTER_ALPHA * corrected +
        (1.0f - ContinuousConfig::GYRO_FILTER_ALPHA) *
          filteredGyro_[i];
    }
  }

  filterInitialised_ = true;

  if (elapsed == 0) {
    return;
  }

  if (elapsed > ContinuousConfig::MAX_SAMPLE_GAP_MS) {
    phase_ = WAITING;
    endSettling_ = false;
    quietActive_ = false;
    previousCycleEnded_ = false;
    return;
  }

  // ----------------------------------------------------
  // Dominant gyro-axis discovery
  // ----------------------------------------------------

  if (axis_ < 0) {
    ++axisSamples_;

    for (int i = 0; i < 3; ++i) {
      axisEnergy_[i] +=
        filteredGyro_[i] * filteredGyro_[i];
    }

    if (
      now - axisWindowStart_ <
      ContinuousConfig::AXIS_WINDOW_MS
    ) {
      return;
    }

    int strongest = 0;

    for (int i = 1; i < 3; ++i) {
      if (axisEnergy_[i] > axisEnergy_[strongest]) {
        strongest = i;
      }
    }

    const float totalEnergy =
      axisEnergy_[0] + axisEnergy_[1] + axisEnergy_[2];

    const float rms = sqrtf(
      axisEnergy_[strongest] / axisSamples_
    );

    const float fraction =
      totalEnergy > 0.0f
        ? axisEnergy_[strongest] / totalEnergy
        : 0.0f;

    if (
      rms >= ContinuousConfig::MIN_AXIS_RMS &&
      fraction >= ContinuousConfig::MIN_AXIS_ENERGY_FRACTION
    ) {
      axis_ = strongest;
    } else {
      for (int i = 0; i < 3; ++i) {
        axisEnergy_[i] = 0.0f;
      }

      axisSamples_ = 0;
      axisWindowStart_ = now;
      return;
    }
  }

  signal_ = filteredGyro_[axis_];

  // ----------------------------------------------------
  // Track true quiet intervals
  // ----------------------------------------------------

  if (stationary) {
    if (!quietActive_) {
      quietActive_ = true;
      quietStart_ = now;
    }

    if (
      phase_ != WAITING &&
      now - quietStart_ >= GeneralConfig::QUIET_TIME_MS
    ) {
      cycleHadPause_ = true;
    }
  } else {
    quietActive_ = false;
  }

  if (firstSign_ == 0.0f) {
    if (fabsf(signal_) < startGate_) {
      return;
    }

    firstSign_ = signal_ >= 0.0f ? 1.0f : -1.0f;
  }

  // Positive means the initial learned direction.
  const float directedRate = signal_ * firstSign_;
  const float dt = elapsed / 1000.0f;

  if (phase_ == WAITING) {
    if (directedRate < startGate_) {
      return;
    }

    startCycle(now);
  }

  if (now - cycleStart_ > maxDuration_) {
    phase_ = WAITING;
    endSettling_ = false;
    previousCycleEnded_ = true;
    previousCycleEnd_ = now;
    return;
  }

  // ----------------------------------------------------
  // First half-cycle
  // ----------------------------------------------------

  if (phase_ == FIRST_DIRECTION) {
    if (directedRate > 0.0f) {
      firstAngle_ += directedRate * dt;

      if (directedRate > firstPeak_) {
        firstPeak_ = directedRate;
      }
    }

    if (directedRate <= -startGate_) {
      phase_ = SECOND_DIRECTION;
      secondStart_ = now;
      endSettling_ = false;

      secondAngle_ += -directedRate * dt;
      secondPeak_ = -directedRate;
    }

    return;
  }

  // ----------------------------------------------------
  // Opposite half-cycle
  // ----------------------------------------------------

  if (directedRate < 0.0f) {
    secondAngle_ += -directedRate * dt;

    if (-directedRate > secondPeak_) {
      secondPeak_ = -directedRate;
    }
  }

  if (fabsf(directedRate) <= endGate_) {
    if (!endSettling_) {
      endSettling_ = true;
      endSettleStart_ = now;
    }
  } else {
    endSettling_ = false;
  }

  const bool nextCycleStarting =
    directedRate >= startGate_;

  const bool secondHalfSettled =
    endSettling_ &&
    now - endSettleStart_ >=
      ContinuousConfig::END_SETTLE_MS;

  if (nextCycleStarting || secondHalfSettled) {
    finishCycle(now);

    // Continuous motion may already be entering the next cycle.
    if (nextCycleStarting) {
      startCycle(now);

      firstAngle_ = directedRate * dt;
      firstPeak_ = directedRate;
    }
  }
}

void ContinuousCounter::finishCycle(unsigned long now) {
  const unsigned long duration = now - cycleStart_;

  const unsigned long firstTime =
    secondStart_ - cycleStart_;

  const unsigned long secondTime =
    now - secondStart_;

  const float angle =
    (firstAngle_ + secondAngle_) * 0.5f;

  const float peak =
    firstPeak_ > secondPeak_ ? firstPeak_ : secondPeak_;

  lastAngle_ = angle;
  lastDuration_ = duration;

  const bool valid =
    firstAngle_ >= requiredAngle_ &&
    secondAngle_ >= requiredAngle_ &&
    firstTime >= ContinuousConfig::MIN_HALF_TIME_MS &&
    secondTime >= ContinuousConfig::MIN_HALF_TIME_MS &&
    duration >= minDuration_ &&
    duration <= maxDuration_ &&
    peak >= requiredPeak_;

  if (valid) {
    if (!calibrated_) {
      const bool eligible =
        !requireContinuousCalibration_ || !cycleHadPause_;

      if (eligible) {
        acceptCalibrationCandidate(peak, angle, duration);
      }
    } else {
      ++repetitions_;
      repEvent_ = true;
    }
  }

  phase_ = WAITING;
  endSettling_ = false;

  previousCycleEnded_ = true;
  previousCycleEnd_ = now;
}

bool ContinuousCounter::similar(
  float first,
  float second,
  float tolerance
) {
  const float average = (first + second) * 0.5f;

  return
    average > 0.0f &&
    fabsf(first - second) / average <= tolerance;
}

void ContinuousCounter::acceptCalibrationCandidate(
  float peak,
  float angle,
  unsigned long duration
) {
  calibrationEvent_ = true;

  if (calibrationCount_ == 0) {
    candidatePeak_ = peak;
    candidateAngle_ = angle;
    candidateDuration_ = duration;

    calibrationCount_ = 1;
    return;
  }

  const bool matches =
    similar(
      peak,
      candidatePeak_,
      ContinuousConfig::PEAK_SIMILARITY
    ) &&
    similar(
      angle,
      candidateAngle_,
      ContinuousConfig::ANGLE_SIMILARITY
    ) &&
    similar(
      (float)duration,
      (float)candidateDuration_,
      ContinuousConfig::TIME_SIMILARITY
    );

  if (!matches) {
    // Discard the older candidate; keep the latest one.
    candidatePeak_ = peak;
    candidateAngle_ = angle;
    candidateDuration_ = duration;

    calibrationCount_ = 1;
    return;
  }

  const float averagePeak =
    (candidatePeak_ + peak) * 0.5f;

  const float averageAngle =
    (candidateAngle_ + angle) * 0.5f;

  const float averageDuration =
    (candidateDuration_ + duration) * 0.5f;

  startGate_ = averagePeak * 0.20f;
  if (startGate_ < 0.35f) startGate_ = 0.35f;

  endGate_ = averagePeak * 0.08f;
  if (endGate_ < 0.12f) endGate_ = 0.12f;

  requiredAngle_ = averageAngle * 0.60f;
  if (requiredAngle_ < ContinuousConfig::MIN_HALF_ANGLE) {
    requiredAngle_ = ContinuousConfig::MIN_HALF_ANGLE;
  }

  requiredPeak_ = averagePeak * 0.50f;

  minDuration_ = (unsigned long)(averageDuration * 0.60f);
  maxDuration_ = (unsigned long)(averageDuration * 1.80f);

  if (minDuration_ < 350) minDuration_ = 350;
  if (maxDuration_ > 8000) maxDuration_ = 8000;

  calibrated_ = true;
  calibrationCount_ = 2;
  repetitions_ = 0;
}

bool ContinuousCounter::consumeRepEvent() {
  const bool event = repEvent_;
  repEvent_ = false;
  return event;
}

bool ContinuousCounter::consumeCalibrationEvent() {
  const bool event = calibrationEvent_;
  calibrationEvent_ = false;
  return event;
}