#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>
#include "ExerciseConfig.h"

struct TelemetryData {
  int reps;
  int calibrationReps;
  int calibrationTarget;
  int repState;
  int mode;
  int gyroAxis;
  bool positioningSeen;
  bool buzzer;
};

void printTelemetry(Print& output, const TelemetryData& data);

class WirelessStreamer {
 public:
  WirelessStreamer();
  bool begin(const char* deviceName);
  void update(const TelemetryData& data, unsigned long now);

 private:
  BluetoothSerial bluetooth_;
  unsigned long lastOutputTime_;
};
