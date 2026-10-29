#pragma once

#include <stdint.h>

enum FallDetectorState {
  MONITORING_NORMAL = 0,
  POSSIBLE_FREE_FALL = 1,
  WAITING_FOR_IMPACT = 2
};

class FallDetector {
 public:
  FallDetector();

  void begin(unsigned long currentTime);

  void update(
    float rawAccelerationMagnitude,
    unsigned long currentTime
  );

  bool consumeFallEvent();

  FallDetectorState state() const;

 private:
  FallDetectorState state_;

  unsigned long freeFallStartTime_;
  unsigned long impactWindowStartTime_;

  bool fallEvent_;

  void reset();
};
