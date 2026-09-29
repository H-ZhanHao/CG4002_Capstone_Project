#include "WirelessStreamer.h"

void printTelemetry(Print& output, const TelemetryData& data) {
  output.print("Mode: ");
  output.print(data.mode == PAUSE_MODE ? "PAUSE" : "CONTINUOUS");

  if (data.repState == 0) {
    output.print(data.positioningSeen
      ? " | BASELINE | Hold still for 3 seconds"
      : " | BASELINE | Move into your starting position");
  } else if (data.repState == 1) {
    output.print(" | CALIBRATING: ");
    output.print(data.calibrationReps);
    output.print('/');
    output.print(data.calibrationTarget);
    output.print(" | Reps: -- | ");

    if (data.mode == CONTINUOUS_MODE && data.gyroAxis < 0) {
      output.print("Start moving to learn the movement axis");
    } else {
      output.print(data.mode == PAUSE_MODE
        ? "Repeat evenly; pause between reps"
        : "Repeat evenly at a steady pace");
    }
  } else {
    output.print(" | COUNTING | Reps: ");
    output.print(data.reps);
  }

  if (data.buzzer) {
    output.print(" | FALL ALERT - BUZZER ON");
  }

  output.println();
}

WirelessStreamer::WirelessStreamer()
  : lastOutputTime_(0) {
}

bool WirelessStreamer::begin(const char* deviceName) {
  lastOutputTime_ = 0;
  return bluetooth_.begin(deviceName);
}

void WirelessStreamer::update(const TelemetryData& data, unsigned long now) {
  if (
    now - lastOutputTime_ < GeneralConfig::OUTPUT_INTERVAL_MS
  ) {
    return;
  }

  lastOutputTime_ = now;

  if (!bluetooth_.hasClient()) {
    return;
  }

  printTelemetry(bluetooth_, data);
}
