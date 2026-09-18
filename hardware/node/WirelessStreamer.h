#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "FallDetector.h"
#include "ExerciseRecognition.h"

// Common numeric output for USB Serial and Bluetooth.
// Numeric values keep the format suitable for Serial Plotter.
void printTelemetry(
  Print& output,
  float rawMagnitude,
  float filteredMagnitude,
  const RepCounter& repCounter,
  const FallDetector& fallDetector,
  const ExerciseRecognition& recognition,
  bool buzzerActive
);

class WirelessStreamer {
 public:
  WirelessStreamer();

  bool begin(const char* deviceName);

  void update(
    float rawMagnitude,
    float filteredMagnitude,
    const RepCounter& repCounter,
    const FallDetector& fallDetector,
    const ExerciseRecognition& recognition,
    bool buzzerActive,
    unsigned long currentTime
  );

 private:
  BluetoothSerial bluetooth_;
  unsigned long lastOutputTime_;
};