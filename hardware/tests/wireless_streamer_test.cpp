#include <assert.h>
#include <deque>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "WirelessStreamer.h"

// Fake only the native transports; exercise the production parser/formatter.
static std::deque<int> usbInput, btInput;
static std::string usbOutput, btOutput;
static bool connected = false;
static uint32_t session = 0;
UsbConsole usbConsole;

static int readByte(std::deque<int>& input) {
  if (input.empty()) return -1;
  int value = input.front();
  input.pop_front();
  return value;
}
static void feed(std::deque<int>& input, const std::string& text) {
  for (unsigned char c : text) input.push_back(c);
}
int UsbConsole::read() { return readByte(usbInput); }
size_t UsbConsole::write(const uint8_t* data, size_t size) {
  usbOutput.append(reinterpret_cast<const char*>(data), size);
  return size;
}
bool BluetoothSpp::begin(const char*) { return true; }
bool BluetoothSpp::hasClient() const { return connected; }
uint32_t BluetoothSpp::connectionId() const { return session; }
void BluetoothSpp::service() {}
int BluetoothSpp::read() { return readByte(btInput); }
size_t BluetoothSpp::write(const uint8_t* data, size_t size) {
  btOutput.append(reinterpret_cast<const char*>(data), size);
  return size;
}
void NodeStream::printf(const char* format, ...) {
  char buffer[384];
  va_list args;
  va_start(args, format);
  int size = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  assert(size >= 0 && static_cast<size_t>(size) < sizeof(buffer));
  write(reinterpret_cast<const uint8_t*>(buffer), size);
}

int main() {
  WirelessStreamer streamer;
  assert(streamer.begin("Test"));
  char command[32];
  feed(usbInput, "ACT");
  assert(!streamer.pollCommand(command, sizeof(command)));
  feed(usbInput, "IVATE\r\nSTATUS\n");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "ACTIVATE");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "STATUS");
  assert(!streamer.pollCommand(command, sizeof(command)));

  feed(usbInput, std::string(90, 'A') + "\nHELP\n");
  assert(!streamer.pollCommand(command, sizeof(command))); // Per-loop byte budget.
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "#OVERFLOW");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "HELP");

  connected = true;
  ++session;
  feed(btInput, "ACT");
  assert(!streamer.pollCommand(command, sizeof(command)));
  // A fast reconnect must not join an old partial command to new bytes.
  ++session;
  feed(btInput, "IVATE\n");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "IVATE");
  feed(btInput, "ACT");
  assert(!streamer.pollCommand(command, sizeof(command)));
  btInput.push_back(-2); // Driver detected dropped RX bytes.
  feed(btInput, "IVATE\nSTATUS\n");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "#OVERFLOW");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "STATUS");

  feed(usbInput, "HELP\n");
  feed(btInput, "RESET\n");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "HELP");
  assert(streamer.pollCommand(command, sizeof(command)) && std::string(command) == "RESET");
  streamer.message("hello");
  assert(usbOutput == "hello\r\n" && btOutput == "hello\r\n");

  TelemetryData data = {};
  data.source = HIP;
  data.repState = NODE_STANDBY;
  data.mode = PAUSE_MODE;
  printTelemetry(usbConsole, data);
  assert(usbOutput.find("Source: HIP") != std::string::npos);
  assert(usbOutput.find("STANDBY") != std::string::npos);
  puts("Native stream parser, overflow, reconnect and telemetry tests passed");
}
