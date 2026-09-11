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
  baselineMagnitude_ = 9.81;
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

  resetMovementCycle();

  lastMovementEndTime_ =
    currentTime;

  if (state_ == MOVEMENT_CALIBRATION) {
    if (
      completedDuration <
      GeneralConfig::
        CALIBRATION_MIN_DURATION_MS
    ) {
      return;
    }

    if (
      completedDuration >
      GeneralConfig::
        CALIBRATION_MAX_DURATION_MS
    ) {
      return;
    }

    if (
      completedPeak <
      settings_.minimumCalibrationPeak
    ) {
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
    return;
  }

  if (
    completedDuration <
    minimumRepDuration_
  ) {
    return;
  }

  if (
    completedDuration >
    maximumRepDuration_
  ) {
    return;
  }

  repetitionCount_++;
  repEvent_ = true;
}

void RepCounter::addCalibrationCandidate(
  float peak,
  unsigned long duration
) {
  calibrationPeaks_[calibrationCount_] =
    peak;

  calibrationDurations_[calibrationCount_] =
    duration;

  calibrationCount_++;

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

float RepCounter::pairDifferenceScore(
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
    return 1000000.0;
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
    relativePeakDifference +
    relativeDurationDifference
  );
}

void RepCounter::retainCalibrationPair(
  int first,
  int second
) {
  float firstPeak =
    calibrationPeaks_[first];

  float secondPeak =
    calibrationPeaks_[second];

  unsigned long firstDuration =
    calibrationDurations_[first];

  unsigned long secondDuration =
    calibrationDurations_[second];

  calibrationPeaks_[0] = firstPeak;
  calibrationPeaks_[1] = secondPeak;

  calibrationDurations_[0] =
    firstDuration;

  calibrationDurations_[1] =
    secondDuration;

  calibrationCount_ = 2;
}

void RepCounter::
retainNewestCalibrationCandidate() {
  calibrationPeaks_[0] =
    calibrationPeaks_[2];

  calibrationDurations_[0] =
    calibrationDurations_[2];

  calibrationCount_ = 1;
}

void RepCounter::
evaluateCalibrationCandidates() {
  bool similar01 =
    movementsAreSimilar(0, 1);

  bool similar02 =
    movementsAreSimilar(0, 2);

  bool similar12 =
    movementsAreSimilar(1, 2);

  // All three must agree with each other
  if (
    similar01 &&
    similar02 &&
    similar12
  ) {
    finishCalibration();
    return;
  }

  // Otherwise, keep the closest matching pair
  float bestScore = 1000000.0;

  int bestFirst = -1;
  int bestSecond = -1;

  if (similar01) {
    bestScore =
      pairDifferenceScore(0, 1);

    bestFirst = 0;
    bestSecond = 1;
  }

  if (similar02) {
    float score =
      pairDifferenceScore(0, 2);

    if (score < bestScore) {
      bestScore = score;
      bestFirst = 0;
      bestSecond = 2;
    }
  }

  if (similar12) {
    float score =
      pairDifferenceScore(1, 2);

    if (score < bestScore) {
      bestScore = score;
      bestFirst = 1;
      bestSecond = 2;
    }
  }

  if (bestFirst >= 0) {
    retainCalibrationPair(
      bestFirst,
      bestSecond
    );
  } else {
    // If nothing matches, retain only
    // the newest movement
    retainNewestCalibrationCandidate();
  }
}

void RepCounter::finishCalibration() {
  float averagePeak =
    (
      calibrationPeaks_[0] +
      calibrationPeaks_[1] +
      calibrationPeaks_[2]
    ) / 3.0;

  float averageDuration =
    (
      calibrationDurations_[0] +
      calibrationDurations_[1] +
      calibrationDurations_[2]
    ) / 3.0;

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

bool RepCounter::consumeRepEvent() {
  bool result = repEvent_;
  repEvent_ = false;

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