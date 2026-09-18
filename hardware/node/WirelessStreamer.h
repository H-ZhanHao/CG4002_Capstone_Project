#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>

#include "ExerciseConfig.h"

struct TelemetryData {
  float raw;
  float filtered;
  float motion;

  // Pause-detector diagnostics.
  float high;
  float low;
  float requiredPeak;
  float lastPeak;
  float lastTimeS;

  // Continuous-detector diagnostics.
  float gyroSignal;
  float gyroHigh;
  float requiredAngle;
  float lastAngle;
  float cycleTimeS;

  int reps;
  int calibrationReps;
  int pauseCandidates;
  int continuousCandidates;

  int repState;
  int mode;
  int requestedMode;
  int gyroAxis;

  int fallState;
  bool buzzer;

  int exercise;
  bool recognitionActive;
  bool recognitionMock;
};

void printTelemetry(
  Print& output,
  const TelemetryData& data
);

class WirelessStreamer {
 public:
  WirelessStreamer();

  bool begin(const char* deviceName);

  void update(
    const TelemetryData& data,
    unsigned long currentTime
  );

 private:
  BluetoothSerial bluetooth_;
  unsigned long lastOutputTime_;
};