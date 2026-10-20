#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NodePlatform {
  unsigned long millis();
  unsigned long micros();
  void delayMs(unsigned long duration);
  void outputPin(int pin);
  void writePin(int pin, int level);
}

class NodeStream {
 public:
  virtual ~NodeStream() = default;
  virtual int read() = 0;
  virtual size_t write(const uint8_t* bytes, size_t length) = 0;
  void println(const char* text);
  void printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
};

class UsbConsole : public NodeStream {
 public:
  void begin(int baud);
  int read() override;
  size_t write(const uint8_t* bytes, size_t length) override;
};
extern UsbConsole usbConsole;

class BluetoothSpp : public NodeStream {
 public:
  bool begin(const char* deviceName);
  bool hasClient() const;
  uint32_t connectionId() const;
  int read() override;
  size_t write(const uint8_t* bytes, size_t length) override;
  void service();
};

