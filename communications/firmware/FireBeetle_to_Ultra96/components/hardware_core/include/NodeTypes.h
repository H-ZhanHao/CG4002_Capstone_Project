#pragma once
#include <stdint.h>

enum CountingMode { PAUSE_MODE = 1, CONTINUOUS_MODE = 2 };
enum NodeRole { HIP = 1, WRIST = 2 };
enum OutputState { WAITING_FOR_PEER = -1, ACTIVATION_FAILED = -2, NODE_STANDBY = -3 };

// Local USB/Bluetooth output only; this struct is never sent over ESP-NOW.
struct TelemetryData {
  uint32_t uptimeMs;
  int32_t reps;
  int32_t calibrationReps;
  int32_t calibrationTarget;
  int32_t repState;
  int32_t mode;
  int32_t gyroAxis;
  float pauseMotion;
  float pausePeak;
  uint8_t buzzer;
  uint8_t source;
  uint8_t exercise;
};

