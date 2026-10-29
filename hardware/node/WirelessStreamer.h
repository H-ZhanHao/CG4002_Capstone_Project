#pragma once

#include "NodePlatform.h"
#include "ExerciseConfig.h"

void printTelemetry(NodeStream& output, const TelemetryData& data);

class WirelessStreamer {
 public:
  WirelessStreamer();
  bool begin(const char* deviceName);
  void update(const TelemetryData& data, unsigned long now);
  bool pollCommand(char* destination, size_t capacity);
  void message(const char* text);
  bool consumeConnectionEvent();

 private:
  struct CommandBuffer {
    char text[128] = {}; // Coordinated starts include boot/arm/session/timestamp fields.
    size_t length = 0;
    bool overflow = false;
  };

  BluetoothSpp bluetooth_;
  uint32_t bluetoothSession_ = 0;
  unsigned long lastOutputTime_;
  bool wasConnected_ = false;
  bool started_ = false;
  bool connectionEvent_ = false;
  CommandBuffer usbInput_;
  CommandBuffer bluetoothInput_;

  bool readCommand(NodeStream& input, CommandBuffer& buffer,
                   char* destination, size_t capacity);
};
