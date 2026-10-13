#include "WirelessStreamer.h"
#include "ExerciseProfiles.h"

#include <stdio.h>
#include <string.h>

namespace {
  size_t formatTelemetry(char* line, size_t capacity, const TelemetryData& data) {
    char status[160];

    if (data.repState == WAITING_FOR_PEER) {
      snprintf(status, sizeof(status), "ACTIVATING | Reps: -- | Waiting for peer standby");
    } else if (data.repState == ACTIVATION_FAILED) {
      snprintf(status, sizeof(status), "ACTIVATION FAILED | Reps: -- | Check other node; send ACTIVATE to retry");
    } else if (data.repState == NODE_STANDBY) {
      snprintf(status, sizeof(status), "STANDBY | Reps: -- | Send ACTIVATE to start");
    } else if (data.repState == 0) {
      snprintf(status, sizeof(status), "BASELINE | %s", data.positioningSeen
        ? "Hold still for 3 seconds"
        : "Move into your starting position");
    } else if (data.repState == 1) {
      const char* instruction = data.mode == PAUSE_MODE
        ? "Repeat evenly; pause between reps"
        : data.gyroAxis < 0
          ? "Start moving to learn the movement axis"
          : "Repeat evenly at a steady pace";
      if (data.mode == PAUSE_MODE) {
        snprintf(status, sizeof(status),
          "CALIBRATING: %d/%d | Motion: %.2f | Peak: %.2f m/s2 | Reps: -- | %s",
          data.calibrationReps, data.calibrationTarget,
          data.pauseMotion, data.pausePeak, instruction);
      } else {
        snprintf(status, sizeof(status), "CALIBRATING: %d/%d | Reps: -- | %s",
          data.calibrationReps, data.calibrationTarget, instruction);
      }
    } else {
      if (data.mode == PAUSE_MODE) {
        snprintf(status, sizeof(status),
          "COUNTING | Reps: %d | Motion: %.2f | Peak: %.2f m/s2",
          data.reps, data.pauseMotion, data.pausePeak);
      } else {
        snprintf(status, sizeof(status), "COUNTING | Reps: %d", data.reps);
      }
    }

    const int length = snprintf(line, capacity, "Up: %lus | Source: %s | Exercise: %s | Mode: %s | %s%s\r\n",
      (unsigned long)(data.uptimeMs / 1000),
      data.source == WRIST ? "WRIST" : "HIP",
      exerciseName((ExerciseType)data.exercise),
      data.mode == PAUSE_MODE ? "PAUSE" : "CONTINUOUS",
      status,
      data.buzzer ? " | FALL ALERT - BUZZER ON" : "");

    return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
  }
}

void printTelemetry(Print& output, const TelemetryData& data) {
  char line[320];
  const size_t length = formatTelemetry(line, sizeof(line), data);
  if (length > 0) {
    output.write((const uint8_t*)line, length);
  }
}

WirelessStreamer::WirelessStreamer()
  : lastOutputTime_(0) {
}

bool WirelessStreamer::begin(const char* deviceName) {
  lastOutputTime_ = 0;
  wasConnected_ = false;
  usbInput_ = CommandBuffer{};
  bluetoothInput_ = CommandBuffer{};
  started_ = bluetooth_.begin(deviceName);
  return started_;
}

bool WirelessStreamer::consumeConnectionEvent() {
  const bool result = connectionEvent_;
  connectionEvent_ = false;
  return result;
}

bool WirelessStreamer::readCommand(
  Stream& input, CommandBuffer& buffer, char* destination, size_t capacity
) {
  if (capacity == 0) {
    return false;
  }

  // Limit work per loop so partial commands never hold up sensor sampling.
  unsigned int budget = 64;
  while (input.available() > 0 && budget-- > 0) {
    const int value = input.read();
    if (value < 0) {
      break;
    }

    if (value == '\r' || value == '\n') {
      if (buffer.length == 0 && !buffer.overflow) {
        continue;
      }

      buffer.text[buffer.length] = '\0';
      const char* command = buffer.overflow || buffer.length >= capacity
        ? "#OVERFLOW" : buffer.text;
      snprintf(destination, capacity, "%s", command);
      buffer = CommandBuffer{};
      return true;
    }

    if (buffer.overflow) {
      continue;
    }

    if (buffer.length + 1 >= sizeof(buffer.text)) {
      // Discard the entire oversized line, including anything after its prefix.
      buffer.overflow = true;
    } else {
      buffer.text[buffer.length++] = (char)value;
    }
  }

  return false;
}

bool WirelessStreamer::pollCommand(char* destination, size_t capacity) {
  const bool connected = started_ && bluetooth_.hasClient();
  if (!connected) {
    bluetoothInput_ = CommandBuffer{};
  }

  if (readCommand(Serial, usbInput_, destination, capacity)) {
    return true;
  }

  return connected &&
    readCommand(bluetooth_, bluetoothInput_, destination, capacity);
}

void WirelessStreamer::message(const char* text) {
  char line[224];
  const int length = snprintf(line, sizeof(line), "%s\r\n", text);
  if (length <= 0 || (size_t)length >= sizeof(line)) {
    return;
  }

  Serial.write((const uint8_t*)line, (size_t)length);
  if (started_ && bluetooth_.hasClient()) {
    bluetooth_.write((const uint8_t*)line, (size_t)length);
  }
}

void WirelessStreamer::update(const TelemetryData& data, unsigned long now) {
  if (now - lastOutputTime_ < GeneralConfig::OUTPUT_INTERVAL_MS) {
    return;
  }

  lastOutputTime_ = now;
  const bool connected = started_ && bluetooth_.hasClient();
  if (connected != wasConnected_) {
    message(connected
      ? "[Laptop Bluetooth connected]"
      : "[Bluetooth disconnected; USB output remains active]");
    connectionEvent_ = connected;
    wasConnected_ = connected;
  }

  if (!connected) {
    return;
  }

  // One complete line creates one queued packet instead of many small packets.
  // BluetoothSerial::write can still wait if its internal queue is full.
  char line[320];
  const size_t length = formatTelemetry(line, sizeof(line), data);
  if (length == 0) {
    return;
  }

  const unsigned long started = millis();
  const size_t written = bluetooth_.write((const uint8_t*)line, length);
  const unsigned long elapsed = millis() - started;

  // USB-only diagnostics remain visible when the Bluetooth link has stalled.
  if (written != length || elapsed >= 100) {
    Serial.printf("[Bluetooth send: %u/%u bytes queued, blocked %lu ms]\n",
      (unsigned int)written, (unsigned int)length, elapsed);
  }
}
