#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "FallDetector.h"

class WirelessStreamer {
 public:
  WirelessStreamer();

  bool begin(
    const char* deviceName
  );

  void update(
    float rawMagnitude,
    float filteredMagnitude,
    const RepCounter& repCounter,
    const FallDetector& fallDetector,
    bool buzzerActive,
    ExerciseType selectedExercise,
    unsigned long currentTime
  );

 private:
  BluetoothSerial bluetooth_;

  unsigned long lastOutputTime_;

  static constexpr unsigned long
    OUTPUT_INTERVAL_MS = 100;
};