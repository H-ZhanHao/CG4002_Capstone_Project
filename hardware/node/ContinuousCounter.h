#pragma once

#include <Arduino.h>
#include "ExerciseConfig.h"
#include "ExerciseProfiles.h"

class ContinuousCounter {
 public:
  void begin(
    const float gyroBias[3],
    unsigned long now,
    bool requireContinuousCalibration
  );

  void update(
    const float gyro[3],
    bool stationary,
    unsigned long now
  );

  bool consumeRepEvent();
  ContinuousCalibration calibration() const;
  bool restoreCalibration(const ContinuousCalibration& calibration);
  bool consumeCalibrationEvent();

  bool calibrated() const { return calibrated_; }
  int calibrationCount() const { return calibrationCount_; }
  int repetitionCount() const { return repetitions_; }

  int axis() const { return axis_; }
  float signal() const { return signal_; }

  float startGate() const { return startGate_; }
  float requiredAngle() const { return requiredAngle_; }

  float lastAngle() const { return lastAngle_; }
  unsigned long lastDuration() const { return lastDuration_; }

 private:
  enum Phase {
    WAITING,
    FIRST_DIRECTION,
    SECOND_DIRECTION
  };

  bool started_ = false;
  bool calibrated_ = false;
  bool requireContinuousCalibration_ = true;

  int calibrationCount_ = 0;
  int repetitions_ = 0;

  bool repEvent_ = false;
  bool calibrationEvent_ = false;

  float bias_[3] = {};
  float filteredGyro_[3] = {};
  float axisEnergy_[3] = {};

  bool filterInitialised_ = false;
  int axis_ = -1;
  unsigned int axisSamples_ = 0;

  unsigned long axisWindowStart_ = 0;
  unsigned long lastUpdate_ = 0;

  float signal_ = 0.0f;
  float firstSign_ = 0.0f;

  Phase phase_ = WAITING;

  unsigned long cycleStart_ = 0;
  unsigned long secondStart_ = 0;

  bool endSettling_ = false;
  unsigned long endSettleStart_ = 0;

  bool quietActive_ = false;
  unsigned long quietStart_ = 0;
  bool cycleHadPause_ = false;

  bool previousCycleEnded_ = false;
  unsigned long previousCycleEnd_ = 0;

  float firstAngle_ = 0.0f;
  float secondAngle_ = 0.0f;
  float firstPeak_ = 0.0f;
  float secondPeak_ = 0.0f;

  float startGate_ = ContinuousConfig::INITIAL_START_GATE;
  float endGate_ = ContinuousConfig::INITIAL_END_GATE;
  float requiredAngle_ = ContinuousConfig::MIN_HALF_ANGLE;
  float requiredPeak_ = 0.0f;

  unsigned long minDuration_ =
    ContinuousConfig::MIN_CYCLE_TIME_MS;

  unsigned long maxDuration_ =
    ContinuousConfig::MAX_CYCLE_TIME_MS;

  float candidatePeak_ = 0.0f;
  float candidateAngle_ = 0.0f;
  unsigned long candidateDuration_ = 0;

  float lastAngle_ = 0.0f;
  unsigned long lastDuration_ = 0;

  void startCycle(unsigned long now);
  void finishCycle(unsigned long now);

  void acceptCalibrationCandidate(
    float peak,
    float angle,
    unsigned long duration
  );

  static bool similar(
    float first,
    float second,
    float tolerance
  );
};
