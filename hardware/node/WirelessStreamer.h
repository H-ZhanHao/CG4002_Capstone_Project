#pragma once

#include <Arduino.h>
#include <BluetoothSerial.h>
#include "ExerciseConfig.h"

struct TelemetryData {
  unsigned long uptimeMs;
  int reps;
  int calibrationReps;
  int calibrationTarget;
  int repState;
  int mode;
  int gyroAxis;
  float pauseMotion;
  float pausePeak;
  bool positioningSeen;
  bool buzzer;
};

void printTelemetry(Print& output, const TelemetryData& data);

class WirelessStreamer {
 public:
  WirelessStreamer();
  bool begin(const char* deviceName);
  void update(const TelemetryData& data, unsigned long now);
  bool pollCommand(char* destination, size_t capacity);
  void message(const char* text);

 private:
  struct CommandBuffer {
    char text[32] = {};
    size_t length = 0;
    bool overflow = false;
  };

  BluetoothSerial bluetooth_;
  unsigned long lastOutputTime_;
  bool wasConnected_ = false;
  bool started_ = false;
  CommandBuffer usbInput_;
  CommandBuffer bluetoothInput_;

  bool readCommand(Stream& input, CommandBuffer& buffer,
                   char* destination, size_t capacity);
};
