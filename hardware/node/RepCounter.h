#pragma once

#include <Arduino.h>
#include "ExerciseConfig.h"

enum CounterState {
  BASELINE_CALIBRATION = 0,
  MOVEMENT_CALIBRATION = 1,
  REP_COUNTING = 2
};

class RepCounter {
 public:
  explicit RepCounter(
    const ExerciseSettings& settings
  );

  void begin(unsigned long currentTime);

  void update(
    float filteredMagnitude,
    unsigned long currentTime
  );

  bool consumeRepEvent();

  CounterState state() const;

  float motionSignal() const;
  float baselineMagnitude() const;

  float highThreshold() const;
  float lowThreshold() const;
  float requiredRepPeak() const;

  float lastCompletedPeak() const;

  unsigned long minimumRepDuration() const;
  unsigned long maximumRepDuration() const;
  unsigned long lastCompletedDuration() const;

  int repetitionCount() const;
  int calibrationCount() const;

 private:
  ExerciseSettings settings_;
  CounterState state_;

  unsigned long baselineStartTime_;
  float baselineSum_;
  unsigned int baselineSamples_;
  float baselineMagnitude_;
  float motionSignal_;

  float calibrationPeaks_[
    GeneralConfig::REQUIRED_STABLE_REPS
  ];

  unsigned long calibrationDurations_[
    GeneralConfig::REQUIRED_STABLE_REPS
  ];

  int calibrationCount_;

  float highThreshold_;
  float lowThreshold_;
  float requiredRepPeak_;

  unsigned long minimumRepDuration_;
  unsigned long maximumRepDuration_;

  bool movementActive_;
  float movementPeak_;

  unsigned long movementStartTime_;
  unsigned long quietStartTime_;
  unsigned long lastMovementEndTime_;

  float lastCompletedPeak_;
  unsigned long lastCompletedDuration_;

  int repetitionCount_;
  bool repEvent_;

  void resetMovementCycle();

  void processMovement(
    float startThreshold,
    float endThreshold,
    unsigned long currentTime
  );

  void completeMovement(
    unsigned long currentTime
  );

  void addCalibrationCandidate(
    float peak,
    unsigned long duration
  );

  void evaluateCalibrationCandidates();

  bool movementsAreSimilar(
    int first,
    int second
  ) const;

  float pairDifferenceScore(
    int first,
    int second
  ) const;

  void retainCalibrationPair(
    int first,
    int second
  );

  void retainNewestCalibrationCandidate();

  void finishCalibration();
};