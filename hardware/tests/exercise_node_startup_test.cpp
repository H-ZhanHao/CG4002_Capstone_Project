#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <string>
#include "ExerciseNode.h"
#include "ExerciseProfiles.h"
#include "Mpu6050.h"
#include "NodePlatform.h"
#include "NodeRadio.h"

// Test the production command, session and sampling loop with fake hardware.
namespace ExerciseNode { TelemetryData makeTelemetry(); }
static unsigned long now = 0;
static std::deque<int> input;
static std::string output;
static std::deque<LinkPacket> incoming;
static bool peerOnline = true;
static bool sensorHealthy = true;
static float accelZ = 9.81f, gyroX = 0.0f;
static unsigned int readyEvents = 0, repEvents = 0, sampleReads = 0;
static void peerSend(const LinkPacket& packet) { incoming.push_back(packet); }
static void ignoreLog(const char*) {}
static NodeSession peer(peerSend, ignoreLog);
UsbConsole usbConsole;

namespace NodePlatform {
unsigned long millis() { return now; }
unsigned long micros() { return now * 1000; }
void delayMs(unsigned long duration) { now += duration; }
void outputPin(int) {}
void writePin(int, int) {}
}
void UsbConsole::begin(int) {}
int UsbConsole::read() {
  if (input.empty()) return -1;
  const int value = input.front();
  input.pop_front();
  return value;
}
size_t UsbConsole::write(const uint8_t* data, size_t size) {
  output.append(reinterpret_cast<const char*>(data), size);
  return size;
}
void NodeStream::println(const char* text) { printf("%s\r\n", text); }
void NodeStream::printf(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  const int size = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  assert(size >= 0 && static_cast<size_t>(size) < sizeof(buffer));
  write(reinterpret_cast<const uint8_t*>(buffer), size);
}
bool BluetoothSpp::begin(const char*) { return true; }
bool BluetoothSpp::hasClient() const { return false; }
uint32_t BluetoothSpp::connectionId() const { return 0; }
int BluetoothSpp::read() { return -1; }
size_t BluetoothSpp::write(const uint8_t*, size_t size) { return size; }
void BluetoothSpp::service() {}
bool Mpu6050::begin() { return true; }
bool Mpu6050::read(float acceleration[3], float gyro[3]) {
  ++sampleReads;
  if (!sensorHealthy) return false;
  acceleration[0] = acceleration[1] = gyro[1] = gyro[2] = 0;
  acceleration[2] = accelZ;
  gyro[0] = gyroX;
  return true;
}
namespace NodeRadio {
bool begin(NodeRole role) {
  incoming.clear();
  peer.begin(role == HIP ? WRIST : HIP, 100);
  return true;
}
bool ready() { return true; }
void send(const LinkPacket& packet) {
  if (packet.kind == PacketKind::LED_FEEDBACK) {
    readyEvents += packet.mode == static_cast<uint8_t>(LedFeedback::READY);
    repEvents += packet.mode == static_cast<uint8_t>(LedFeedback::REP);
  }
  if (peerOnline) peer.receive(packet, now);
}
bool receive(LinkPacket& packet) {
  if (incoming.empty()) return false;
  packet = incoming.front();
  incoming.pop_front();
  return true;
}
void update() {}
}

static void step(int samples, float acceleration = 9.81f, float gyro = 0.0f) {
  accelZ = acceleration;
  gyroX = gyro;
  for (int i = 0; i < samples; ++i) {
    now += 20;
    ExerciseNode::update();
  }
}
static void command(const char* text) {
  while (*text) input.push_back(*text++);
  input.push_back('\n');
  now += 20;
  ExerciseNode::update();
}
static TelemetryData data() { return ExerciseNode::makeTelemetry(); }
static void pauseRep() { step(25, 11.4f); step(45); }

int main() {
  ExerciseNode::begin(HIP);
  step(10, 11.4f);
  assert(data().reps == 0 && readyEvents == 0 && sampleReads > 0);
  assert(output.find("STANDBY") != std::string::npos);

  // Removing standstill must not bypass the peer acknowledgement.
  peerOnline = false;
  command("ACTIVATE");
  step(130, 11.4f);
  assert(data().reps == 0 && readyEvents == 0);
  assert(output.find("ACTIVATION FAILED") != std::string::npos);
  peerOnline = true;
  command("ACTIVATE"); // Still moving when the peer acknowledges.
  assert(data().repState == 2 && data().reps == 0 && readyEvents == 1);
  assert(peer.feedbackLedOn(now + LedTiming::READY_STEP_MS));
  const unsigned long activatedAt = now;
  pauseRep();
  assert(now - activatedAt < 3000 && data().reps == 1 && repEvents == 1);
  command("ACTIVATE");
  command("EXERCISE:SQUAT");
  assert(data().reps == 1 && readyEvents == 1); // Idempotent commands.
  command("EXERCISE:SITUP");
  assert(data().exercise == SQUAT && data().reps == 1); // Wrong board rejected.

  command("RESET");
  assert(data().repState == 2 && data().reps == 0 && readyEvents == 2);
  pauseRep();
  assert(data().reps == 1);
  command("EXERCISE:LUNGE");
  assert(data().exercise == LUNGE && data().repState == 2 && data().reps == 0);
  assert(readyEvents == 3);
  pauseRep();
  assert(data().reps == 1);
  command("MODE:CONTINUOUS");
  assert(data().repState == 1 && data().reps == 0 && data().gyroAxis == -1);
  command("MODE:PAUSE");
  assert(data().repState == 2 && data().reps == 0);

  command("STANDBY");
  command("RESET");
  const unsigned int readyBefore = readyEvents;
  const unsigned int readsBefore = sampleReads;
  pauseRep();
  assert(data().reps == 0 && readyEvents == readyBefore && sampleReads > readsBefore);
  assert(output.find("Hold still") == std::string::npos);
  assert(output.find("Starting baseline") == std::string::npos);

  // The wrist's default starts axis discovery directly from moving samples.
  ExerciseNode::begin(WRIST);
  gyroX = 3.0f;
  command("ACTIVATE");
  assert(data().exercise == JUMPING_JACK && data().repState == 1);
  step(65, 9.81f, 3.0f);
  assert(data().gyroAxis == 0 && data().repState == 2);
  step(35, 9.81f, -3.0f);
  step(25);
  assert(data().reps == 1);
  const unsigned int readyBeforeSwitch = readyEvents;
  command("EXERCISE:SITUP");
  assert(data().exercise == SITUP && data().mode == PAUSE_MODE);
  assert(data().repState == 2 && data().reps == 0);
  assert(readyEvents == readyBeforeSwitch + 1); // Exercise/mode restart coalesced.
  pauseRep();
  assert(data().reps == 1);

  // Both board roles retain samples while their counters are in standby.
  static SensorSample window[SensorHistoryConfig::CAPACITY];
  for (NodeRole role : {HIP, WRIST}) {
    ExerciseNode::begin(role);
    const size_t capacity = SensorHistoryConfig::CAPACITY;
    assert(ExerciseNode::copySensorWindow((uint32_t)now, window, capacity).status == SensorWindowStatus::EMPTY);
    step(10);
    output.clear();
    command("WINDOW");
    assert(output.find("INSUFFICIENT_HISTORY") != std::string::npos);
    step(250, 10.0f, 0.5f);
    auto captured = ExerciseNode::copySensorWindow((uint32_t)now, window, capacity);
    assert(captured.status == SensorWindowStatus::READY && captured.count == 251);
    assert(window[captured.count - 1].acceleration[2] == 10.0f);
    assert(window[captured.count - 1].gyro[0] == 0.5f);
    assert(data().reps == 0);
    output.clear();
    command("BUFFER");
    command("WINDOW");
    assert(output.find(role == HIP ? "[BUFFER | HIP" : "[BUFFER | WRIST") != std::string::npos);
    assert(output.find(" | READY | ") != std::string::npos);
    assert(output.find("samples=251") != std::string::npos);

    // Counter commands neither clear history nor skip scheduled sensor reads.
    const uint32_t anchor = (uint32_t)now;
    command("ACTIVATE");
    command("RESET");
    command(role == HIP ? "EXERCISE:LUNGE" : "EXERCISE:SITUP");
    command("MODE:CONTINUOUS");
    command("STANDBY");
    assert(ExerciseNode::copySensorWindow(anchor, window, capacity).status == SensorWindowStatus::READY);
    captured = ExerciseNode::copySensorWindow((uint32_t)now, window, capacity);
    assert(captured.status == SensorWindowStatus::READY && captured.maxGapMs == 20);
    assert(captured.count == 251);
    char historicalCommand[32];
    snprintf(historicalCommand, sizeof(historicalCommand), "WINDOW:%lu", (unsigned long)anchor);
    output.clear();
    command(historicalCommand);
    assert(output.find(" | READY | ") != std::string::npos);
    for (const char* bad : {"WINDOW:", "WINDOW:-1", "WINDOW:4294967296", "WINDOW:12X"}) {
      output.clear();
      command(bad);
      assert(output.find("ERROR: Use WINDOW") != std::string::npos);
    }

    // Failed reads leave a visible gap rather than stale/zero samples.
    sensorHealthy = false;
    const uint32_t beforeFailure = (uint32_t)now;
    step(10);
    assert(ExerciseNode::copySensorWindow((uint32_t)now, window, capacity).status == SensorWindowStatus::END_NOT_AVAILABLE);
    sensorHealthy = true;
    step(1);
    captured = ExerciseNode::copySensorWindow((uint32_t)now, window, capacity);
    assert(captured.status == SensorWindowStatus::SAMPLE_GAP && captured.maxGapMs == 220);
    for (size_t i = 0; i < captured.count; ++i)
      assert(window[i].timestampMs <= beforeFailure || window[i].timestampMs == (uint32_t)now);
  }
  puts("Node startup, activation, reset, exercise/mode switching and LED tests passed");
  puts("Both node buffers, diagnostics, independent sampling and sensor-failure tests passed");
}
