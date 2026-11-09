#pragma once

#include <stdint.h>

enum FallDetectorState {
  MONITORING_NORMAL = 0,
  POSSIBLE_FREE_FALL = 1,
  WAITING_FOR_IMPACT = 2,
  CONFIRMING_FALL = 3
};

class FallDetector {
 public:
  FallDetector();

  void begin(unsigned long currentTime);

  void update(
    const float acceleration[3],
    const float gyro[3],
    unsigned long currentTime
  );

  bool consumeFallEvent();

  FallDetectorState state() const;

 private:
  FallDetectorState state_;

  uint32_t freeFallStartTime_;
  uint32_t impactWindowStartTime_;
  uint32_t impactTime_;
  uint32_t lastSampleTime_;
  uint32_t quietStartTime_;
  uint32_t settledStartTime_;
  float gravity_[3];
  float postImpactGravity_[3];
  bool haveSample_;
  bool haveGravity_;
  bool trackingQuiet_;
  bool trackingSettled_;

  bool fallEvent_;

  void reset();
};
