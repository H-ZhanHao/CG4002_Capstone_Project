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
static unsigned int radioPackets = 0;
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
    assert(event.gyroRadS > 2 && data().reps == 0 && data().repState == 2);

    // An early peak is observable; it cannot manufacture five seconds of history.
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
    assert(firstWindow.info.window.endMs == peak.peakMs && firstWindow.info.window.count == 251);
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
    assert(scheduled.info.window.startMs == peak.peakMs - 4000);
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
    assert(data().repState == 2);
    command("STREAM:START");
    assert(data().repState == 2 && radioPackets == packetsAfterActivation);
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
    assert(data().repState == 2);
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
  puts("Node startup, activation, reset, exercise/mode switching and LED tests passed");
  puts("Both node buffers, diagnostics, independent sampling and sensor-failure tests passed");
  puts("Both node peak triggers, window timestamps and counting/radio independence tests passed");
  puts("Both node coordinated starts, deactivation, stream controls, overflow and outage tests passed");
}
