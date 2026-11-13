#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <string>
#include "ExerciseNode.h"
#include "ExerciseProfiles.h"
#include "ExerciseConfig.h"
#include "BuzzerFeedback.h"
#include "WirelessStreamer.h"
#include "Mpu6050.h"
#include "NodePlatform.h"
#include "NodeRadio.h"

// Test the production command, session and sampling loop with fake hardware.
namespace ExerciseNode { TelemetryData makeTelemetry(); }
static unsigned long now = 0;
static std::deque<int> input, btInput;
static bool btConnected = false;
static uint32_t btSession = 0;
static std::string btOutput;
static std::string output;
static std::deque<LinkPacket> incoming;
static bool peerOnline = true;
static bool sensorHealthy = true;
static float accelZ = 9.81f, gyroX = 0.0f;
static unsigned int readyEvents = 0, repEvents = 0, sampleReads = 0;
static unsigned int radioPackets = 0;
static int ledLevel = 0, buzzerLevel = 0;
static unsigned int buzzerWrites = 0;
static void peerSend(const LinkPacket& packet) { incoming.push_back(packet); }
static void ignoreLog(const char*) {}
static unsigned int peerRepBeeps = 0, peerRepClears = 0;
static void peerFeedback(RepFeedback kind) {
  if (kind == RepFeedback::REP) ++peerRepBeeps;
  else ++peerRepClears;
}
static NodeSession peer(peerSend, ignoreLog, peerFeedback);
UsbConsole usbConsole;

namespace NodePlatform {
unsigned long millis() { return now; }
unsigned long micros() { return now * 1000; }
void delayMs(unsigned long duration) { now += duration; }
void outputPin(int) {}
void writePin(int pin, int level) {
  if (pin == GeneralConfig::LED_PIN) ledLevel = level;
  if (pin == FallConfig::BUZZER_PIN) { buzzerLevel = level; ++buzzerWrites; }
}
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
bool BluetoothSpp::hasClient() const { return btConnected; }
uint32_t BluetoothSpp::connectionId() const { return btSession; }
int BluetoothSpp::read() {
  if (btInput.empty()) return -1;
  int value = btInput.front(); btInput.pop_front(); return value;
}
size_t BluetoothSpp::write(const uint8_t* bytes, size_t size) {
  btOutput.append(reinterpret_cast<const char*>(bytes), size); return size;
}
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
  ++radioPackets;
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
  do {
    now += 20;
    ExerciseNode::update();
  } while (!input.empty());
}
static StreamStart acceptTrigger(uint32_t session) {
  StreamTrigger trigger;
  assert(ExerciseNode::pollStreamTrigger(trigger));
  StreamStart start;
  start.node = trigger.node;
  start.coordinator = 17;
  start.session = session;
  start.localT0 = trigger.peakMs;
  assert(ExerciseNode::joinWindowStream(start) == StreamJoinResult::STARTED);
  return start;
}
static TelemetryData data() { return ExerciseNode::makeTelemetry(); }
static void pauseRep() { step(25, 11.4f); step(45); }
static void genericPulse(bool gyroOnly = false) {
  const float values[] = {0, .4f, .8f, 1.2f, 1.6f, 2, 2.4f, 2.4f,
                         2, 1.5f, 1, .5f, 0, 0, 0, 0, 0, 0};
  for (float value : values)
    step(1, 9.81f + (gyroOnly ? 0 : value), gyroOnly ? value : 0);
}

struct RepTicket { uint32_t boot, session; };
static RepTicket ticket() {
  command("REPS");
  const size_t at = output.rfind("[REPS | ");
  assert(at != std::string::npos);
  unsigned long boot, session;
  assert(sscanf(output.c_str() + at, "[REPS | %*[^|]| boot=%lu | session=%lu", &boot, &session) == 2);
  return {static_cast<uint32_t>(boot), static_cast<uint32_t>(session)};
}
static void sendReps(RepTicket target, uint32_t total, bool bluetooth = false) {
  char line[96];
  snprintf(line, sizeof(line), "REPS:%lu:%lu:%lu", (unsigned long)target.boot,
    (unsigned long)target.session, (unsigned long)total);
  if (!bluetooth) { command(line); return; }
  for (const char* c = line; *c; ++c) btInput.push_back(*c);
  btInput.push_back('\n');
  do { step(1); } while (!btInput.empty());
}

static void sendActivity(uint32_t boot, uint32_t sequence, const char* state, bool bluetooth = false) {
  char line[96];
  snprintf(line, sizeof(line), "ACTIVITY:%lu:%lu:%s", (unsigned long)boot,
    (unsigned long)sequence, state);
  if (!bluetooth) { command(line); return; }
  for (const char* c = line; *c; ++c) btInput.push_back(*c);
  btInput.push_back('\n');
  do { step(1); } while (!btInput.empty());
}

static void activityTests() {
  for (NodeRole role : {HIP, WRIST}) {
    peerOnline = false;
    ExerciseNode::begin(role);
    const auto initial = ExerciseNode::activityStatus();
    assert(initial.boot && initial.state == UserActivity::IDLE);
    assert(!initial.received && initial.sequence == 0);
    assert(ledLevel == GeneralConfig::LED_OFF);
    output.clear(); command("ACTIVITY");
    assert(output.find("state=IDLE | source=DEFAULT") != std::string::npos);
    const auto radioBefore = radioPackets, readyBefore = readyEvents, repsBefore = repEvents;
    const auto readsBefore = sampleReads;

    // Comms can describe user movement on a standby board, even with no peer.
    sendActivity(initial.boot, 1, "ACTIVE");
    assert(ExerciseNode::activityStatus().state == UserActivity::ACTIVE);
    assert(ExerciseNode::activityStatus().received);
    assert(output.find("state=ACTIVE | ACCEPTED") != std::string::npos);
    output.clear(); command("STATUS");
    assert(output.find("state=ACTIVE | source=COMMS") != std::string::npos);
    assert(output.find("[Standby |") != std::string::npos);
    assert(ledLevel == GeneralConfig::LED_ON && buzzerLevel == 0);
    for (int i = 0; i < 80; ++i) {
      step(1);
      assert(ledLevel == GeneralConfig::LED_ON); // Steady, including standby.
    }
    assert(ExerciseNode::activityStatus().state == UserActivity::ACTIVE); // No inactivity inference.
    sendReps(ticket(), 1);
    assert(data().reps == 0); // Activity ACTIVE does not activate rep reception.

    sendActivity(initial.boot, 1, "ACTIVE");
    assert(output.find("state=ACTIVE | DUPLICATE") != std::string::npos);
    sendActivity(initial.boot, 1, "IDLE");
    assert(output.find("state=IDLE | SEQUENCE_CONFLICT") != std::string::npos);
    assert(ledLevel == GeneralConfig::LED_ON);
    sendActivity(initial.boot, 3, "IDLE");
    sendActivity(initial.boot, 2, "ACTIVE");
    assert(output.find("state=ACTIVE | STALE_SEQUENCE") != std::string::npos);
    sendActivity(initial.boot + 1, 4, "ACTIVE");
    assert(output.find("state=ACTIVE | STALE_BOOT") != std::string::npos);
    sendActivity(initial.boot, 0, "ACTIVE");
    assert(output.find("state=ACTIVE | INVALID_SEQUENCE") != std::string::npos);
    assert(ExerciseNode::receiveActivity(initial.boot, 4, static_cast<UserActivity>(99))
      == ActivityResult::INVALID_STATE);
    for (const char* bad : {"ACTIVITY:", "ACTIVITY:1:2", "ACTIVITY:1:2:REST",
         "ACTIVITY:1:2:ACTIVE:EXTRA", "ACTIVITY:1:2:3:ACTIVE", "ACTIVITY:1::ACTIVE",
         "ACTIVITY:1:-1:IDLE", "ACTIVITY:1:4294967296:ACTIVE", "ACTIVITY:4294967296:1:IDLE"}) {
      output.clear(); command(bad);
      assert(output.find("ERROR: Use ACTIVITY:") != std::string::npos);
      assert(ExerciseNode::activityStatus().sequence == 3);
      assert(ExerciseNode::activityStatus().state == UserActivity::IDLE);
      assert(ledLevel == GeneralConfig::LED_OFF);
    }
    pauseRep();
    assert(ExerciseNode::activityStatus().state == UserActivity::IDLE);
    assert(radioPackets == radioBefore && readyEvents == readyBefore && repEvents == repsBefore);
    assert(sampleReads > readsBefore && data().reps == 0 && !data().buzzer);

    // Activity does not select an exercise, reset reps, or change counting activation.
    peerOnline = true; command("ACTIVATE");
    sendReps(ticket(), 5);
    assert(ExerciseNode::activityStatus().state == UserActivity::IDLE);
    assert(data().reps == 5);
    assert(ledLevel == GeneralConfig::LED_OFF); // No ready/rep flashes while idle.
    sendActivity(initial.boot, 4, "ACTIVE");
    assert(data().reps == 5);
    for (const char* change : {"RESET", role == HIP ? "EXERCISE:LUNGE" : "EXERCISE:SITUP",
                              "MODE:CONTINUOUS", "STANDBY", "ACTIVATE"}) {
      command(change);
      assert(ExerciseNode::activityStatus().sequence == 4);
      assert(ExerciseNode::activityStatus().state == UserActivity::ACTIVE);
      assert(ledLevel == GeneralConfig::LED_ON);
    }
    sendReps(ticket(), 1);
    assert(ledLevel == GeneralConfig::LED_ON);
    // A peer running older firmware can still send pulses; ignore them at GPIO.
    peer.activate(now);
    step(2);
    peer.showLedFeedback(LedFeedback::READY, now);
    for (int i = 0; i < 40; ++i) {
      step(1);
      assert(ledLevel == GeneralConfig::LED_ON);
    }

    // Bluetooth input uses the same validation, including retries after reconnect.
    btConnected = true; ++btSession; btOutput.clear();
    sensorHealthy = false;
    sendActivity(initial.boot, 5, "IDLE", true);
    assert(btOutput.find("state=IDLE | ACCEPTED") != std::string::npos);
    assert(ExerciseNode::activityStatus().state == UserActivity::IDLE);
    assert(ledLevel == GeneralConfig::LED_OFF); // Also works with failed sensor reads.
    peer.showLedFeedback(LedFeedback::REP, now);
    step(1);
    assert(ledLevel == GeneralConfig::LED_OFF);
    sensorHealthy = true;
    btConnected = false; step(1); btConnected = true; ++btSession;
    sendActivity(initial.boot, 5, "IDLE", true);
    assert(btOutput.find("state=IDLE | DUPLICATE") != std::string::npos);
    sendActivity(initial.boot, 6, "ACTIVE", true);
    btConnected = false;
    step(60);
    assert(ExerciseNode::activityStatus().state == UserActivity::ACTIVE);
    assert(ledLevel == GeneralConfig::LED_ON);
    sendActivity(initial.boot, UINT32_MAX, "IDLE");
    sendActivity(initial.boot, 1, "ACTIVE"); // Sequence numbers do not wrap within a boot.
    assert(ExerciseNode::activityStatus().sequence == UINT32_MAX);
    assert(ExerciseNode::activityStatus().state == UserActivity::IDLE);

    ExerciseNode::begin(role);
    assert(!ExerciseNode::activityStatus().received && ExerciseNode::activityStatus().sequence == 0);
    sendActivity(initial.boot, UINT32_MAX, "ACTIVE");
    assert(ExerciseNode::activityStatus().state == UserActivity::IDLE);
    sendActivity(ExerciseNode::activityStatus().boot, 1, "ACTIVE");
    assert(ExerciseNode::activityStatus().received);
  }
  puts("Activity input, ordering, reboot/reconnect and independence tests passed for HIP/WRIST");
}

static void buzzerTests() {
  // Rollover and a delayed main loop must retain a real silent gap.
  BuzzerFeedback feedback;
  feedback.begin();
  feedback.rep(UINT32_MAX - 49);
  feedback.rep(UINT32_MAX - 20);
  feedback.update(49);
  assert(feedback.on());
  feedback.update(50);
  assert(!feedback.on());
  feedback.update(150);
  assert(feedback.on());
  feedback.update(1000);
  assert(!feedback.on());
  feedback.rep(1000);
  assert(!feedback.on());
  feedback.update(1100);
  assert(feedback.on());
  feedback.fall(1100);
  feedback.cancelReps();
  feedback.update(4099);
  assert(feedback.on());
  feedback.update(4100);
  assert(!feedback.on());

  peerOnline = true;
  sensorHealthy = true;
  btConnected = false;
  ExerciseNode::begin(HIP);
  command("ACTIVATE");
  auto current = ticket();
  sendReps(current, 0);
  assert(!buzzerLevel);
  const auto reads = sampleReads;
  sendReps(current, 1);
  assert(buzzerLevel && data().buzzer);
  output.clear();
  printTelemetry(usbConsole, data());
  assert(output.find("REP BEEP - BUZZER ON") != std::string::npos);
  assert(output.find("FALL ALERT") == std::string::npos);
  step(4);
  assert(buzzerLevel && sampleReads > reads); // 80 ms, sampling never blocked.
  sendReps(current, 1); // Duplicate at 100 ms cannot restart the beep.
  assert(!buzzerLevel && !data().buzzer);
  step(10);
  assert(!buzzerLevel);
  sendReps(current, 0);
  sendReps({current.boot + 1, current.session}, 2);
  assert(!buzzerLevel);

  // Bluetooth input and failed sensor reads still service the buzzer timer.
  btConnected = true; ++btSession;
  sensorHealthy = false;
  sendReps(current, 5, true); // Catch-up total yields one confirmation beep.
  assert(buzzerLevel);
  step(5);
  assert(!buzzerLevel);
  step(15);
  assert(!buzzerLevel);
  sensorHealthy = true; btConnected = false;

  sendReps(current, 6);
  sendReps(current, 7); // Queue a separate beep, don't extend the first.
  step(4);
  assert(!buzzerLevel);
  step(5);
  assert(buzzerLevel);
  step(5);
  assert(!buzzerLevel);
  command("RESET");
  current = ticket();
  sendReps(current, 1);
  sendReps(current, 2);
  command("RESET");
  assert(!buzzerLevel);
  step(20);
  assert(!buzzerLevel); // Reset removes the queued beep too.
  current = ticket();
  sendReps(current, 1);
  command("STANDBY");
  assert(!buzzerLevel);
  sendReps(current, 2);
  assert(!buzzerLevel);

  command("ACTIVATE");
  current = ticket();
  step(6, 0.0f); // Qualifying freefall.
  sendReps(current, 1); // Still in freefall, rep beep is active at impact.
  step(1, 30.0f);
  const auto fallAt = now;
  assert(buzzerLevel);
  output.clear();
  printTelemetry(usbConsole, data());
  assert(output.find("FALL ALERT - BUZZER ON") != std::string::npos);
  sendReps(current, 2); // Suppressed, never queued behind the alarm.
  command("RESET");
  command("STANDBY");
  assert(buzzerLevel); // Rep/session operations cannot silence a fall.
  while (now - fallAt < FallConfig::BUZZER_DURATION_MS - 20) {
    step(1);
    assert(buzzerLevel);
  }
  step(1);
  assert(!buzzerLevel);
  step(20);
  assert(!buzzerLevel);

  const auto beforeWrist = buzzerWrites;
  ExerciseNode::begin(WRIST);
  command("ACTIVATE");
  sendReps(ticket(), 1);
  step(20);
  assert(buzzerWrites == beforeWrist && !data().buzzer);
  puts("Single rep beeps, queue/gap, duplicate rejection, fall priority and HIP-only GPIO tests passed");
}

static void remoteBuzzerTests() {
  peerOnline = true; sensorHealthy = true; btConnected = false;
  ExerciseNode::begin(HIP);
  const auto boot = ExerciseNode::activityStatus().boot;
  sendActivity(boot, 1, "ACTIVE");
  peer.activate(now); step(2);
  assert(peer.localActive() && data().reps == 0);
  peer.showRepFeedback(now);
  assert(!incoming.empty());
  const auto remoteEvent = incoming.front();
  sensorHealthy = false;
  step(1);
  assert(buzzerLevel && data().buzzer && !data().fallAlert);
  assert(data().reps == 0 && ledLevel == GeneralConfig::LED_ON);
  incoming.push_back(remoteEvent); // Lost ACK/retry cannot extend the beep.
  step(5);
  assert(!buzzerLevel);
  sensorHealthy = true;
  peer.showRepFeedback(now); peer.showRepFeedback(now);
  step(6); assert(buzzerLevel); // Respect the silent gap after the previous beep.
  peer.clearRepFeedback(now); step(1);
  assert(!buzzerLevel);
  step(20); assert(!buzzerLevel); // Remote reset cancels queued beeps too.

  step(6, 0.0f); step(1, 30.0f);
  const auto fallAt = now;
  assert(buzzerLevel && data().fallAlert);
  peer.showRepFeedback(now); step(1);
  peer.clearRepFeedback(now); step(1);
  assert(buzzerLevel && data().fallAlert);
  while (now - fallAt < FallConfig::BUZZER_DURATION_MS) step(1);
  assert(!buzzerLevel);
  step(20); assert(!buzzerLevel); // Suppressed remote rep is never replayed.

  peer.showRepFeedback(now); step(1); assert(buzzerLevel);
  command("ACTIVATE");
  assert(!buzzerLevel && data().repState == COMMS_WAITING);
  incoming.push_back(remoteEvent); step(1);
  assert(!buzzerLevel && data().reps == 0);

  ExerciseNode::begin(WRIST);
  command("ACTIVATE");
  const auto current = ticket();
  const auto beepsBefore = peerRepBeeps, clearsBefore = peerRepClears;
  sendReps(current, 0);
  assert(peerRepBeeps == beepsBefore);
  sendReps(current, 5);
  assert(peerRepBeeps == beepsBefore + 1 && data().reps == 5);
  sendReps(current, 5); sendReps(current, 4);
  sendReps({current.boot + 1, current.session}, 6);
  assert(peerRepBeeps == beepsBefore + 1 && data().reps == 5);
  command("RESET");
  assert(peerRepClears == clearsBefore + 1 && data().reps == 0);
  puts("WRIST comms-to-radio and HIP standby buzzer/LED/count independence, reset and fall priority passed");
}

int main() {
  remoteBuzzerTests();
  buzzerTests();
  activityTests();
  readyEvents = repEvents = 0;
  // The firmware never counts acceleration/gyro cycles; only comms totals count.
  for (NodeRole role : {HIP, WRIST}) {
    ExerciseNode::begin(role);
    const auto inactive = ticket();
    sendReps(inactive, 1);
    assert(output.find("| INACTIVE]") != std::string::npos && data().reps == 0);
    peerOnline = false;
    command("ACTIVATE");
    step(130, 11.4f);
    assert(output.find("ACTIVATION FAILED") != std::string::npos);
    sendReps(inactive, 1);
    assert(data().reps == 0);
    peerOnline = true;
    command("ACTIVATE");
    assert(data().repState == COMMS_WAITING && data().reps == 0);
    assert(!peer.feedbackLedOn(now + LedTiming::READY_STEP_MS));
    auto current = ticket();
    const auto ledsBefore = repEvents;
    pauseRep();
    step(65, 9.81f, 3.0f); step(35, 9.81f, -3.0f); step(25);
    assert(data().reps == 0 && repEvents == ledsBefore && data().gyroAxis == -1);
    sendReps(current, 0);
    assert(data().repState == COMMS_COUNTING && repEvents == ledsBefore);
    sendReps(current, 5);
    assert(data().reps == 5 && repEvents == ledsBefore);
    assert(!peer.feedbackLedOn(now) && ledLevel == GeneralConfig::LED_OFF);
    step(11);
    assert(!peer.feedbackLedOn(now));
    assert(output.find("COUNTING | Reps: 5 | Rep source: COMMS") != std::string::npos);
    assert(output.find("FINDING AXIS") == std::string::npos);
    sendReps(current, 5);
    assert(output.find("| DUPLICATE]") != std::string::npos && repEvents == ledsBefore);
    sendReps(current, 4);
    assert(output.find("| DECREASING]") != std::string::npos && data().reps == 5);
    sendReps({current.boot + 1, current.session}, 6);
    sendReps({current.boot, current.session + 1}, 6);
    assert(output.find("| STALE_SESSION]") != std::string::npos && data().reps == 5);
    sendReps(current, 2147483648U);
    assert(output.find("| OUT_OF_RANGE]") != std::string::npos && data().reps == 5);
    for (const char* bad : {"REPS:", "REPS:1:2", "REPS:1:2:3:4", "REPS:1:2:-1",
                           "REPS:1:2:4294967296", "REPS:1:2:3X", "REPS:1::3"}) {
      output.clear(); command(bad);
      assert(output.find("ERROR: Use REPS") != std::string::npos && data().reps == 5);
    }
    // Repeating activation/selection preserves the current ticket and count.
    command("ACTIVATE");
    command(role == HIP ? "EXERCISE:SQUAT" : "EXERCISE:JUMPING_JACK");
    command(role == HIP ? "EXERCISE:SITUP" : "EXERCISE:SQUAT"); // Wrong board.
    assert(ticket().session == current.session && data().reps == 5);

    // Reset, exercise and actual mode changes invalidate earlier results.
    for (const char* change : {"RESET", role == HIP ? "EXERCISE:LUNGE" : "EXERCISE:SITUP", "MODE:CONTINUOUS"}) {
      command(change);
      const auto next = ticket();
      assert(next.session != current.session && data().reps == 0 && data().repState == COMMS_WAITING);
      sendReps(current, 6);
      assert(data().reps == 0);
      sendReps(next, 2);
      assert(data().reps == 2);
      current = next;
    }
    command("MODE:CONTINUOUS"); // Retry does not reset.
    assert(ticket().session == current.session && data().reps == 2);

    // Full Bluetooth -> parser -> count -> USB and Bluetooth diagnostic path.
    btConnected = true; ++btSession;
    btOutput.clear();
    sensorHealthy = false;
    sendReps(current, 7, true);
    assert(data().reps == 7 && btOutput.find("| ACCEPTED]") != std::string::npos);
    sensorHealthy = true;
    step(6);
    assert(btOutput.find("COUNTING | Reps: 7 | Rep source: COMMS") != std::string::npos);
    btConnected = false; step(1); btConnected = true; ++btSession;
    sendReps(current, 7, true); // Reconnect and retry does not reset or flash.
    assert(btOutput.find("| DUPLICATE]") != std::string::npos);
    btConnected = false;
    command("STANDBY");
    const auto stoppedLeds = repEvents;
    sendReps(current, 8);
    assert(data().reps == 7 && repEvents == stoppedLeds);
    command("ACTIVATE");
    sendReps(current, 8);
    assert(data().reps == 0);
    current = ticket();
    sendReps(current, INT32_MAX);
    assert(data().reps == INT32_MAX);
    ExerciseNode::begin(role);
    command("ACTIVATE");
    sendReps(current, INT32_MAX); // Old boot cannot repopulate a new session.
    assert(data().reps == 0);
  }
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
    assert(captured.status == SensorWindowStatus::READY && captured.count == 151);
    assert(window[captured.count - 1].acceleration[2] == 10.0f);
    assert(window[captured.count - 1].gyro[0] == 0.5f);
    assert(data().reps == 0);
    output.clear();
    command("BUFFER");
    command("WINDOW");
    assert(output.find(role == HIP ? "[BUFFER | HIP" : "[BUFFER | WRIST") != std::string::npos);
    assert(output.find(" | READY | ") != std::string::npos);
    assert(output.find("samples=151") != std::string::npos);

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
    assert(captured.count == 151);
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
  // Both nodes detect in standby with no peer, without activating or sending radio packets.
  for (NodeRole role : {HIP, WRIST}) {
    peerOnline = false;
    ExerciseNode::begin(role);
    GenericPeakEvent event;
    assert(!ExerciseNode::consumeGenericPeak(event));
    step(260);
    output.clear();
    const unsigned int packetsBefore = radioPackets, ledsBefore = readyEvents + repEvents;
    genericPulse();
    const uint32_t firstFinished = (uint32_t)now;
    // Preserve the first unconsumed trigger across commands and later peaks.
    command("RESET");
    command(role == HIP ? "EXERCISE:LUNGE" : "EXERCISE:SITUP");
    command("STANDBY");
    step(60);
    genericPulse(true);
    assert(ExerciseNode::consumeGenericPeak(event));
    assert(event.detectedMs <= firstFinished && event.peakMs < event.detectedMs);
    assert(!ExerciseNode::consumeGenericPeak(event));
    auto captured = ExerciseNode::copySensorWindow(event.peakMs, window, SensorHistoryConfig::CAPACITY);
    assert(captured.status == SensorWindowStatus::READY && captured.lastMs == event.peakMs);
    assert(output.find(role == HIP ? "[PEAK | HIP" : "[PEAK | WRIST") != std::string::npos);
    assert(data().reps == 0 && output.find("STANDBY | Reps: --") != std::string::npos);
    assert(output.find("COUNTING | Reps:") == std::string::npos);
    assert(radioPackets == packetsBefore && readyEvents + repEvents == ledsBefore);

    // Detection remains independent of the active exercise counter.
    peerOnline = true;
    command("ACTIVATE");
    step(60);
    genericPulse(true); // Both selected exercises are in acceleration-based PAUSE mode.
    assert(ExerciseNode::consumeGenericPeak(event));
    assert(event.gyroRadS > 2 && data().reps == 0 && data().repState == COMMS_WAITING);

    // An early peak is observable; it cannot manufacture three seconds of history.
    ExerciseNode::begin(role);
    assert(!ExerciseNode::consumeGenericPeak(event));
    step(5);
    genericPulse(true);
    assert(ExerciseNode::consumeGenericPeak(event));
    assert(ExerciseNode::copySensorWindow(event.peakMs, window, SensorHistoryConfig::CAPACITY).status
           == SensorWindowStatus::INSUFFICIENT_HISTORY);

    // Failed reads cannot confirm a peak that started before the interruption.
    step(60);
    step(8, 12.0f);
    sensorHealthy = false;
    step(10);
    sensorHealthy = true;
    step(10);
    assert(!ExerciseNode::consumeGenericPeak(event));
    genericPulse();
    assert(ExerciseNode::consumeGenericPeak(event));
    // Reboot drops an unconsumed event.
    step(60);
    genericPulse();
    ExerciseNode::begin(role);
    assert(!ExerciseNode::consumeGenericPeak(event));
  }
  // Peaks request a coordinated session; only the accepted START creates windows.
  static ScheduledWindow scheduled, firstWindow;
  for (NodeRole role : {HIP, WRIST}) {
    ExerciseNode::begin(role);
    peerOnline = false;
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::ARMED);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    step(260);
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::ARMED);
    output.clear();
    const unsigned int beforePackets = radioPackets, beforeLeds = readyEvents + repEvents;
    genericPulse();
    assert(ExerciseNode::windowStreamStatus().queued == 0);
    assert(output.find("WAIT_COMMS") != std::string::npos);
    const StreamStart accepted = acceptTrigger(1);
    const auto started = ExerciseNode::windowStreamStatus();
    assert(started.state == WindowStreamState::RUNNING && started.queued == 1);
    GenericPeakEvent peak;
    assert(ExerciseNode::consumeGenericPeak(peak));
    assert(started.triggerMs == peak.peakMs && started.nextEndMs == peak.peakMs + 1000);
    assert(ExerciseNode::takeSensorWindow(firstWindow));
    assert(firstWindow.info.coordinator == 17 && firstWindow.info.session == 1);
    assert(firstWindow.info.sequence == 0 && firstWindow.info.window.status == SensorWindowStatus::READY);
    assert(firstWindow.info.window.endMs == peak.peakMs && firstWindow.info.window.count == 151);
    assert(output.find(role == HIP ? "[FRAME | HIP" : "[FRAME | WRIST") != std::string::npos);
    assert(data().reps == 0 && radioPackets == beforePackets && readyEvents + repEvents == beforeLeds);
    assert(ExerciseNode::joinWindowStream(accepted) == StreamJoinResult::ALREADY_RUNNING);
    assert(!ExerciseNode::takeSensorWindow(scheduled)); // Retry cannot recreate sequence zero.

    // No new peak is required. Service follows the original t0, even in standby.
    while ((uint32_t)now + 20 < started.nextEndMs) step(1);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    step(1);
    assert(ExerciseNode::takeSensorWindow(scheduled));
    assert(scheduled.info.sequence == 1 && scheduled.info.window.endMs == started.nextEndMs);
    assert(scheduled.info.window.startMs == peak.peakMs - 2000);
    assert(firstWindow.info.window.endMs == peak.peakMs); // Caller-owned snapshot remains unchanged.
    command("RESET");
    command("STANDBY");
    command(role == HIP ? "EXERCISE:LUNGE" : "EXERCISE:SITUP");
    command("STREAM:ARM"); // Re-arming a running stream must not move its anchor.
    const auto unchanged = ExerciseNode::windowStreamStatus();
    assert(unchanged.session == started.session && unchanged.triggerMs == started.triggerMs);
    genericPulse(true);
    assert(ExerciseNode::windowStreamStatus().triggerMs == started.triggerMs);

    // A missing consumer fills a bounded queue. The sensor ring still advances.
    step(250);
    assert(ExerciseNode::windowStreamStatus().queued == 3 && ExerciseNode::windowStreamStatus().dropped > 0);
    assert(output.find("QUEUE_FULL") != std::string::npos);
    const auto endBeforeStop = ExerciseNode::windowStreamStatus().nextEndMs;
    output.clear();
    command("STREAM:NEXT");
    assert(output.find("[FRAME TAKEN | ") != std::string::npos);
    assert(ExerciseNode::windowStreamStatus().queued == 2);
    assert(ExerciseNode::windowStreamStatus().nextEndMs == endBeforeStop);
    command("STREAM:STOP");
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::STOPPED);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    step(60);
    genericPulse();
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::STOPPED);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    command("STREAM:ARM");
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::ARMED);
    step(60);
    genericPulse(true);
    acceptTrigger(2);
    assert(ExerciseNode::windowStreamStatus().session == started.session + 1);
    assert(ExerciseNode::takeSensorWindow(scheduled) && scheduled.info.sequence == 0);
    assert(scheduled.info.triggerMs != started.triggerMs);

    // Manual start permits bench testing without a movement; invalid anchors do not start.
    command("STREAM:STOP");
    for (const char* bad : {"STREAM:START:", "STREAM:START:-1", "STREAM:START:4294967296", "STREAM:START:12X"}) {
      output.clear();
      command(bad);
      assert(output.find("ERROR: Use STREAM:START") != std::string::npos);
      assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::STOPPED);
    }
    assert(!ExerciseNode::startWindowStream((uint32_t)now + 1000));
    const uint32_t manualT0 = (uint32_t)now;
    command("STREAM:START");
    assert(ExerciseNode::takeSensorWindow(scheduled) && scheduled.info.triggerMs == manualT0);
    assert(scheduled.info.window.status == SensorWindowStatus::READY);
    command("STREAM:START");
    assert(ExerciseNode::windowStreamStatus().triggerMs == manualT0);

    // Failed reads still service deadlines; recovery yields real samples with a gap flag.
    sensorHealthy = false;
    step(55);
    assert(ExerciseNode::takeSensorWindow(scheduled));
    assert(scheduled.info.sequence == 1 && scheduled.info.window.status == SensorWindowStatus::END_NOT_AVAILABLE);
    sensorHealthy = true;
    step(50);
    assert(ExerciseNode::takeSensorWindow(scheduled));
    assert(scheduled.info.sequence == 2 && scheduled.info.window.status == SensorWindowStatus::SAMPLE_GAP);
    assert(scheduled.info.window.maxGapMs >= 1100);

    // Stream controls do not stop an already activated counter or send peer packets.
    peerOnline = true;
    command("ACTIVATE");
    const unsigned int packetsAfterActivation = radioPackets;
    command("STREAM:STOP");
    assert(data().repState == COMMS_WAITING);
    command("STREAM:START");
    assert(data().repState == COMMS_WAITING && radioPackets == packetsAfterActivation);
    command("STREAM:STOP");

    // Endpoint time can arrive between sensor reads: the non-sampling path services it.
    step(10);
    const uint32_t failingT0 = (uint32_t)now - 11;
    assert(ExerciseNode::startWindowStream(failingT0));
    assert(ExerciseNode::takeSensorWindow(scheduled));
    sensorHealthy = false;
    step(54); // The failed endpoint is now 91 ms overdue.
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    now += 9; // 100 ms endpoint wait elapsed, less than the 20 ms sampling interval.
    const unsigned int readsBeforeTimer = sampleReads;
    ExerciseNode::update();
    assert(sampleReads == readsBeforeTimer);
    assert(ExerciseNode::takeSensorWindow(scheduled));
    assert(scheduled.info.window.status == SensorWindowStatus::END_NOT_AVAILABLE);
    sensorHealthy = true;

    // An early trigger keeps its anchor and produces explicit unavailable-history records.
    ExerciseNode::begin(role);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    step(5);
    genericPulse();
    acceptTrigger(3);
    assert(ExerciseNode::takeSensorWindow(scheduled));
    assert(scheduled.info.sequence == 0 && scheduled.info.window.status == SensorWindowStatus::INSUFFICIENT_HISTORY);
    const uint32_t earlyT0 = scheduled.info.triggerMs;
    for (int second = 0; second < 6; ++second) {
      step(50);
      assert(ExerciseNode::takeSensorWindow(scheduled));
      assert(scheduled.info.triggerMs == earlyT0);
    }
    assert(scheduled.info.window.status == SensorWindowStatus::READY);
    assert(data().reps == 0);
    ExerciseNode::begin(role);
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::ARMED);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
  }
  // Serial comms controls exercise the same typed API, including maximum-width IDs.
  for (NodeRole role : {HIP, WRIST}) {
    ExerciseNode::begin(role);
    step(260);
    output.clear();
    command("STREAM:CLOCK:4294967295");
    assert(output.find("probe=4294967295") != std::string::npos);
    assert(output.find("armed=1") != std::string::npos);
    const auto identity = ExerciseNode::streamIdentity();
    const uint32_t t0 = (uint32_t)now;
    char join[128];
    snprintf(join, sizeof(join), "STREAM:JOIN:%lu:%lu:4294967295:4294967295:%lu",
      (unsigned long)identity.boot, (unsigned long)identity.arm, (unsigned long)t0);
    command(join); // Quiet node can start without detecting a local peak.
    assert(ExerciseNode::takeSensorWindow(scheduled));
    assert(scheduled.info.coordinator == UINT32_MAX && scheduled.info.session == UINT32_MAX);
    assert(scheduled.info.window.endMs == t0);
    const uint32_t before = ExerciseNode::windowStreamStatus().nextEndMs;
    command(join);
    assert(output.find("ALREADY_RUNNING") != std::string::npos);
    assert(ExerciseNode::windowStreamStatus().nextEndMs == before);
    peerOnline = true;
    command(role == HIP ? "EXERCISE:SQUAT" : "EXERCISE:SITUP");
    command("ACTIVATE");
    assert(data().repState == COMMS_WAITING);
    command("DEACTIVATE");
    assert(output.find("rep counting stopped") != std::string::npos);
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::RUNNING);
    step(25, 11.4f); step(45);
    assert(data().reps == 0); // Deactivation stops counting while windows continue.
    command("STREAM:END:1:1");
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::RUNNING);
    command("STREAM:END:4294967295:4294967295");
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::STOPPED);
    assert(!ExerciseNode::takeSensorWindow(scheduled));
    command("STREAM:ARM");
    command(join);
    assert(output.find("STALE_TARGET") != std::string::npos);
    assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::ARMED);
    for (const char* bad : {"STREAM:JOIN:", "STREAM:JOIN:1:2:3:4", "STREAM:JOIN:1:2:3:4:5:6",
                           "STREAM:JOIN:1:2:3:-4:5", "STREAM:JOIN:1:2:3:4:4294967296"}) {
      output.clear();
      command(bad);
      assert(output.find("ERROR: Use STREAM:JOIN") != std::string::npos);
      assert(ExerciseNode::windowStreamStatus().state == WindowStreamState::ARMED);
    }
  }
  puts("Comms counts, USB/Bluetooth reception, stale/duplicate rejection and LED tests passed");
  puts("Both node buffers, diagnostics, independent sampling and sensor-failure tests passed");
  puts("Both node peak triggers, window timestamps and counting/radio independence tests passed");
  puts("Both node coordinated starts, deactivation, stream controls, overflow and outage tests passed");
}
