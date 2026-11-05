#include "Mpu6050.h"
#include "NodePlatform.h"
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <esp_random.h>

#include "ExerciseNode.h"
#include "ExerciseConfig.h"
#include "CommsRepCounter.h"
#include "BuzzerFeedback.h"
#include "ExercisePresets.h"
#include "FallDetector.h"
#include "WirelessStreamer.h"
#include "NodeRadio.h"
#include "NodeSession.h"

namespace ExerciseNode {

using NodePlatform::millis;
using NodePlatform::micros;
using NodePlatform::delayMs;
using NodePlatform::writePin;

NodeRole nodeRole = HIP;
Mpu6050 mpu;
SensorHistory sensorHistory;
GenericPeakDetector genericPeakDetector;
GenericPeakEvent pendingGenericPeak;
bool genericPeakPending = false;
WindowScheduler windowScheduler;
NodeStreamControl streamControl(windowScheduler, sensorHistory);

CommsRepCounter commsReps;
CommsActivityState commsActivity;
ExerciseType activeExercise = SQUAT;

FallDetector fallDetector;
WirelessStreamer wirelessStreamer;

void linkMessage(const char* text) { wirelessStreamer.message(text); }
NodeSession nodeSession(NodeRadio::send, linkMessage);
const char* commandHelp = "Commands: ACTIVATE | STANDBY | RESET | EXERCISE:<name> | EXERCISES | MODE:PAUSE | MODE:CONTINUOUS | BUFFER | WINDOW[:t0_ms] | STREAM | STATUS | HELP. Select Newline, then Send.";
const char* repCommandHelp = "Comms reps: REPS (show ticket/count) | REPS:<boot>:<session>:<total>. ACTIVATE first; use the current HW ticket. Total is cumulative.";
const char* activityCommandHelp = "Activity: ACTIVITY (status) | ACTIVITY:<boot>:<sequence>:IDLE|ACTIVE. Independent of board activation; sequence increases until reboot.";
const char* streamCommandHelp = "Windows: STREAM | STREAM:START[:t0_ms] | STREAM:STOP | STREAM:ARM | STREAM:NEXT. Local preparation only; no comms transmission.";
const char* coordinationHelp = "Comms bench: STREAM:CLOCK:<probe> | STREAM:JOIN:<boot>:<arm>:<coordinator>:<session>:<local_t0> | STREAM:END:<coordinator>:<session> | STREAM:REQUEST";

bool wirelessStarted = false;

CountingMode activeMode = PAUSE_MODE;

unsigned long lastSampleTime = 0;
unsigned long lastUsbOutputTime = 0;

BuzzerFeedback buzzerFeedback;

void writeBuzzer() {
  if (nodeRole == HIP) writePin(FallConfig::BUZZER_PIN, buzzerFeedback.on() ? 1 : 0);
}

void setLed(bool on) {
  writePin(
    GeneralConfig::LED_PIN,
    on ? GeneralConfig::LED_ON : GeneralConfig::LED_OFF
  );
}

void updateActivityLed() {
  setLed(commsActivity.status().state == UserActivity::ACTIVE);
}

void reportCommsReps() {
  char line[192];
  snprintf(line, sizeof(line), "[REPS | %s | boot=%lu | session=%lu | total=%ld | %s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)commsReps.boot(),
    (unsigned long)commsReps.session(), (long)commsReps.total(),
    !nodeSession.localActive() ? "INACTIVE" : commsReps.received() ? "COMMS" : "WAIT_COMMS");
  wirelessStreamer.message(line);
}

ActivityStatus activityStatus() { return commsActivity.status(); }

void reportActivity() {
  const ActivityStatus status = activityStatus();
  char line[192];
  snprintf(line, sizeof(line), "[ACTIVITY | %s | boot=%lu | seq=%lu | state=%s | source=%s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)status.boot,
    (unsigned long)status.sequence, userActivityName(status.state),
    status.received ? "COMMS" : "DEFAULT");
  wirelessStreamer.message(line);
}

ActivityResult receiveActivity(uint32_t boot, uint32_t sequence, UserActivity state) {
  const ActivityResult result = commsActivity.receive(boot, sequence, state);
  updateActivityLed();
  char line[192];
  snprintf(line, sizeof(line), "[ACTIVITY ACK | %s | boot=%lu | seq=%lu | state=%s | %s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)boot, (unsigned long)sequence,
    userActivityName(state), activityResultName(result));
  wirelessStreamer.message(line);
  return result;
}

CommsRepResult receiveRepCount(uint32_t boot, uint32_t session, uint32_t total) {
  const int32_t before = commsReps.total();
  const CommsRepResult result = commsReps.receive(boot, session, total, nodeSession.localActive());
  if (nodeRole == HIP && result == CommsRepResult::ACCEPTED && commsReps.total() > before) {
    buzzerFeedback.rep((uint32_t)millis());
    writeBuzzer();
  }
  char line[192];
  snprintf(line, sizeof(line), "[REPS ACK | %s | boot=%lu | session=%lu | total=%lu | %s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)boot, (unsigned long)session,
    (unsigned long)total, commsRepResultName(result));
  wirelessStreamer.message(line);
  return result;
}

void updateOutputTimers(unsigned long now) {
  updateActivityLed();

  buzzerFeedback.update((uint32_t)now);
  writeBuzzer();
}

void restartSession(unsigned long now) {
  commsReps.reset();
  buzzerFeedback.cancelReps();
  writeBuzzer();
  updateActivityLed();
  reportCommsReps();
  lastUsbOutputTime = now - GeneralConfig::OUTPUT_INTERVAL_MS;
  // Rep resets do not affect sensor history or the independent window stream.
}

SensorWindowResult copySensorWindow(uint32_t endMs, SensorSample* destination, size_t capacity) {
  return sensorHistory.extract(endMs, destination, capacity);
}

bool consumeGenericPeak(GenericPeakEvent& event) {
  if (!genericPeakPending) return false;
  event = pendingGenericPeak;
  genericPeakPending = false;
  return true;
}

WindowStreamStatus windowStreamStatus() { return windowScheduler.status(); }
bool takeSensorWindow(ScheduledWindow& destination) { return windowScheduler.take(destination); }
bool pollStreamTrigger(StreamTrigger& trigger) { return streamControl.pollTrigger((uint32_t)millis(), trigger); }
StreamNodeIdentity streamIdentity() { return streamControl.identity(); }
StreamClockReply streamClockReply(uint32_t probe, uint32_t receivedMs) {
  return streamControl.clockReply(probe, receivedMs, (uint32_t)millis());
}

void reportTrigger() {
  StreamTrigger trigger;
  if (!streamControl.pendingTrigger(trigger)) {
    wirelessStreamer.message("[TRIGGER | NONE]");
    return;
  }
  char line[192];
  snprintf(line, sizeof(line), "[TRIGGER | %s | boot=%lu | arm=%lu | t0_ms=%lu | confirmed_ms=%lu | WAIT_COMMS]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)trigger.node.boot, (unsigned long)trigger.node.arm,
    (unsigned long)trigger.peakMs, (unsigned long)trigger.detectedMs);
  wirelessStreamer.message(line);
}

void reportStreamStatus() {
  const WindowStreamStatus status = windowScheduler.status();
  const char* state = status.state == WindowStreamState::RUNNING ? "RUNNING"
    : status.state == WindowStreamState::ARMED ? "ARMED" : "STOPPED";
  char line[216];
  snprintf(line, sizeof(line),
    "[STREAM | %s | %s | coord=%lu | session=%lu | t0_ms=%lu | next_end_ms=%lu | queue=%u | dropped=%lu | skipped=%lu]",
    nodeRole == HIP ? "HIP" : "WRIST", state, (unsigned long)status.coordinator, (unsigned long)status.session,
    (unsigned long)status.triggerMs, (unsigned long)status.nextEndMs, (unsigned int)status.queued,
    (unsigned long)status.dropped, (unsigned long)status.skipped);
  wirelessStreamer.message(line);
  const StreamNodeIdentity identity = streamControl.identity();
  StreamTrigger pending;
  snprintf(line, sizeof(line), "[COORD | %s | boot=%lu | arm=%lu | %s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)identity.boot, (unsigned long)identity.arm,
    streamControl.pendingTrigger(pending) ? "WAIT_COMMS" : state);
  wirelessStreamer.message(line);
}

void reportPreparedWindow() {
  const WindowPreparation& prepared = windowScheduler.lastPreparation();
  const WindowStreamStatus status = windowScheduler.status();
  const SensorWindowResult& window = prepared.info.window;
  char line[224];
  snprintf(line, sizeof(line),
    "[FRAME | %s | coord=%lu | session=%lu | seq=%lu | %s | start_ms=%lu | end_ms=%lu | samples=%u | queue=%u | drop=%lu | skip=%lu]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)prepared.info.coordinator, (unsigned long)prepared.info.session,
    (unsigned long)prepared.info.sequence, prepared.queued ? sensorWindowStatusName(window.status) : "QUEUE_FULL",
    (unsigned long)window.startMs, (unsigned long)window.endMs, (unsigned int)window.count,
    (unsigned int)status.queued, (unsigned long)status.dropped, (unsigned long)status.skipped);
  wirelessStreamer.message(line);
}

bool startWindowStream(uint32_t t0) {
  if (!streamControl.startLocal(t0, (uint32_t)millis())) return false;
  reportStreamStatus();
  reportPreparedWindow();
  return true;
}

void stopWindowStream() { streamControl.stop(); }
void armWindowStream() { streamControl.arm(); }

StreamJoinResult joinWindowStream(const StreamStart& start) {
  const StreamJoinResult result = streamControl.join(start, (uint32_t)millis());
  char line[200];
  snprintf(line, sizeof(line), "[JOIN | %s | %s | coord=%lu | session=%lu | boot=%lu | arm=%lu | t0_ms=%lu]",
    nodeRole == HIP ? "HIP" : "WRIST", streamJoinResultName(result),
    (unsigned long)start.coordinator, (unsigned long)start.session,
    (unsigned long)start.node.boot, (unsigned long)start.node.arm, (unsigned long)start.localT0);
  wirelessStreamer.message(line);
  if (result == StreamJoinResult::STARTED) {
    reportStreamStatus();
    reportPreparedWindow();
  }
  return result;
}

bool endWindowStream(uint32_t coordinator, uint32_t session) { return streamControl.end(coordinator, session); }

void serviceWindowStream() {
  if (windowScheduler.update((uint32_t)millis(), sensorHistory)) reportPreparedWindow();
}

void detectGenericPeak(const SensorSample& sample) {
  GenericPeakEvent event;
  if (!genericPeakDetector.update(sample, event)) return;
  if (!genericPeakPending) {
    pendingGenericPeak = event;
    genericPeakPending = true;
  }
  // A single coordinator chooses the shared t0; local peaks only request a start.
  const bool requested = streamControl.peak(event);
  // Diagnostic metadata only; no window payload, activation or peer trigger.
  char line[192];
  snprintf(line, sizeof(line),
    "[PEAK | %s | t0_ms=%lu | confirmed_ms=%lu | accel=%.2f m/s2 | gyro=%.2f rad/s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)event.peakMs,
    (unsigned long)event.detectedMs, event.accelerationMps2, event.gyroRadS);
  wirelessStreamer.message(line);
  if (requested) reportTrigger();
}

void reportBuffer(unsigned long now) {
  char line[200];
  if (!sensorHistory.size()) {
    wirelessStreamer.message("[BUFFER | EMPTY | Waiting for a successful sensor sample]");
    return;
  }
  snprintf(line, sizeof(line),
    "[BUFFER | %s | samples=%u/%u | oldest_ms=%lu | newest_ms=%lu | span_ms=%lu | age_ms=%lu]",
    nodeRole == HIP ? "HIP" : "WRIST",
    (unsigned int)sensorHistory.size(), (unsigned int)SensorHistoryConfig::CAPACITY,
    (unsigned long)sensorHistory.oldestMs(), (unsigned long)sensorHistory.newestMs(),
    (unsigned long)(uint32_t)(sensorHistory.newestMs() - sensorHistory.oldestMs()),
    (unsigned long)(uint32_t)((uint32_t)now - sensorHistory.newestMs()));
  wirelessStreamer.message(line);
}

void reportWindow(uint32_t endMs) {
  // Only a small summary is printed; never dump hundreds of samples in update().
  const SensorWindowResult result = sensorHistory.inspect(endMs, WindowSchedulerConfig::SAMPLE_CAPACITY);
  char line[220];
  snprintf(line, sizeof(line),
    "[WINDOW | %s | %s | start_ms=%lu | t0_ms=%lu | samples=%u | first_ms=%lu | last_ms=%lu | max_gap_ms=%lu]",
    nodeRole == HIP ? "HIP" : "WRIST", sensorWindowStatusName(result.status),
    (unsigned long)result.startMs, (unsigned long)result.endMs, (unsigned int)result.count,
    (unsigned long)result.firstMs, (unsigned long)result.lastMs, (unsigned long)result.maxGapMs);
  wirelessStreamer.message(line);
}

bool parseTimestamp(const char* text, uint32_t& value) {
  if (!*text) return false;
  value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const uint32_t digit = (uint32_t)(*text - '0');
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  return true;
}

bool parseFields(const char* text, uint32_t* values, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (*text < '0' || *text > '9') return false;
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
      const uint32_t digit = (uint32_t)(*text++ - '0');
      if (value > (UINT32_MAX - digit) / 10) return false;
      value = value * 10 + digit;
    }
    values[i] = value;
    if (i + 1 == count) return *text == '\0';
    if (*text++ != ':') return false;
  }
  return false;
}

void reportExercise() {
  char message[160];
  const CountingMode selectedMode = nodeSession.selectedMode();
  snprintf(message, sizeof(message), "[Exercise: %s | %s | Rep source: COMMS | Local counting disabled]",
    exerciseName(activeExercise), selectedMode == PAUSE_MODE ? "PAUSE" : "CONTINUOUS");
  wirelessStreamer.message(message);
}

void processCommand(char* command, unsigned long now) {
  char* text = command;
  while (isspace((unsigned char)*text)) {
    ++text;
  }

  size_t length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  for (size_t i = 0; i < length; ++i) {
    text[i] = (char)toupper((unsigned char)text[i]);
  }
  if (length == 0) {
    return;
  }

  if (!strcmp(text, "HELP")) {
    wirelessStreamer.message(commandHelp);
    wirelessStreamer.message(repCommandHelp);
    wirelessStreamer.message(activityCommandHelp);
    wirelessStreamer.message(streamCommandHelp);
    wirelessStreamer.message(coordinationHelp);
    return;
  }
  if (!strcmp(text, "STATUS")) {
    nodeSession.status();
    reportExercise();
    reportCommsReps();
    reportActivity();
    reportStreamStatus();
    wirelessStreamer.message(NodeRadio::ready() ? "[ESP-NOW configured and ready]"
      : "[ESP-NOW unavailable; check peer MAC configuration and USB boot messages]");
    return;
  }
  if (!strcmp(text, "ACTIVITY")) {
    reportActivity();
    return;
  }
  if (!strncmp(text, "ACTIVITY:", 9)) {
    char* stateText = strrchr(text + 9, ':');
    uint32_t fields[2];
    if (stateText) *stateText++ = '\0';
    if (!stateText || !parseFields(text + 9, fields, 2) ||
        (strcmp(stateText, "IDLE") && strcmp(stateText, "ACTIVE"))) {
      wirelessStreamer.message("ERROR: Use ACTIVITY:<boot>:<sequence>:IDLE|ACTIVE; unsigned 32-bit fields, sequence >= 1.");
      return;
    }
    receiveActivity(fields[0], fields[1],
      !strcmp(stateText, "ACTIVE") ? UserActivity::ACTIVE : UserActivity::IDLE);
    return;
  }
  if (!strcmp(text, "REPS")) {
    reportCommsReps();
    return;
  }
  if (!strncmp(text, "REPS:", 5)) {
    uint32_t fields[3];
    if (!parseFields(text + 5, fields, 3)) {
      wirelessStreamer.message("ERROR: Use REPS:<boot>:<session>:<total>, unsigned integers; total <= 2147483647.");
      return;
    }
    receiveRepCount(fields[0], fields[1], fields[2]);
    return;
  }
  if (!strcmp(text, "STREAM")) {
    reportStreamStatus();
    return;
  }
  if (!strcmp(text, "STREAM:REQUEST")) {
    reportTrigger();
    return;
  }
  if (!strncmp(text, "STREAM:CLOCK:", 13)) {
    uint32_t probe;
    if (!parseTimestamp(text + 13, probe)) {
      wirelessStreamer.message("ERROR: Use STREAM:CLOCK:<probe>, an unsigned 32-bit number.");
      return;
    }
    const StreamClockReply reply = streamClockReply(probe, (uint32_t)now);
    char line[200];
    snprintf(line, sizeof(line), "[CLOCK | %s | probe=%lu | boot=%lu | arm=%lu | rx_ms=%lu | tx_ms=%lu | armed=%u]",
      nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)reply.probe,
      (unsigned long)reply.node.boot, (unsigned long)reply.node.arm,
      (unsigned long)reply.receivedMs, (unsigned long)reply.sentMs, reply.armed ? 1u : 0u);
    wirelessStreamer.message(line);
    return;
  }
  if (!strncmp(text, "STREAM:JOIN:", 12)) {
    uint32_t fields[5];
    if (!parseFields(text + 12, fields, 5)) {
      wirelessStreamer.message("ERROR: Use STREAM:JOIN:<boot>:<arm>:<coordinator>:<session>:<local_t0>, unsigned 32-bit numbers.");
      return;
    }
    StreamStart start;
    start.node.role = nodeRole;
    start.node.boot = fields[0];
    start.node.arm = fields[1];
    start.coordinator = fields[2];
    start.session = fields[3];
    start.localT0 = fields[4];
    joinWindowStream(start);
    return;
  }
  if (!strncmp(text, "STREAM:END:", 11)) {
    uint32_t fields[2];
    if (!parseFields(text + 11, fields, 2)) {
      wirelessStreamer.message("ERROR: Use STREAM:END:<coordinator>:<session>, unsigned 32-bit numbers.");
      return;
    }
    wirelessStreamer.message(endWindowStream(fields[0], fields[1]) ? "[END | ACCEPTED]" : "[END | STALE_SESSION]");
    reportStreamStatus();
    return;
  }
  if (!strcmp(text, "STREAM:STOP")) {
    stopWindowStream();
    reportStreamStatus();
    return;
  }
  if (!strcmp(text, "STREAM:ARM")) {
    armWindowStream();
    reportStreamStatus();
    return;
  }
  if (!strcmp(text, "STREAM:START") || !strncmp(text, "STREAM:START:", 13)) {
    uint32_t t0 = sensorHistory.newestMs();
    if (text[12] == ':' && !parseTimestamp(text + 13, t0)) {
      wirelessStreamer.message("ERROR: Use STREAM:START or STREAM:START:<t0_ms>, an unsigned 32-bit local timestamp.");
      return;
    }
    if (text[12] != ':' && !sensorHistory.size()) {
      wirelessStreamer.message("ERROR: No sensor samples yet; use STREAM:ARM to wait for a peak.");
      return;
    }
    if (!startWindowStream(t0))
      wirelessStreamer.message("ERROR: Stream already running or t0 is in the future. STREAM:STOP before starting a new session.");
    return;
  }
  if (!strcmp(text, "STREAM:NEXT")) {
    ScheduledWindowInfo info;
    if (!windowScheduler.discard(info)) {
      wirelessStreamer.message("[FRAME TAKEN | EMPTY]");
      return;
    }
    char line[192];
    snprintf(line, sizeof(line),
      "[FRAME TAKEN | %s | coord=%lu | session=%lu | seq=%lu | %s | start_ms=%lu | end_ms=%lu | samples=%u]",
      nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)info.coordinator, (unsigned long)info.session, (unsigned long)info.sequence,
      sensorWindowStatusName(info.window.status), (unsigned long)info.window.startMs,
      (unsigned long)info.window.endMs, (unsigned int)info.window.count);
    wirelessStreamer.message(line);
    return;
  }
  if (!strcmp(text, "BUFFER")) {
    reportBuffer(now);
    return;
  }
  if (!strcmp(text, "WINDOW")) {
    reportWindow(sensorHistory.newestMs());
    return;
  }
  if (!strncmp(text, "WINDOW:", 7)) {
    uint32_t endMs;
    if (!parseTimestamp(text + 7, endMs)) {
      wirelessStreamer.message("ERROR: Use WINDOW or WINDOW:<t0_ms>, an unsigned 32-bit local timestamp.");
      return;
    }
    reportWindow(endMs);
    return;
  }
  if (!strcmp(text, "#OVERFLOW")) {
    wirelessStreamer.message("ERROR: Command too long. Send HELP for commands.");
    return;
  }
  if (!strcmp(text, "EXERCISES")) {
    wirelessStreamer.message(nodeRole == HIP
      ? "HIP exercises: PUSHUP | SQUAT | LUNGE | LEG_RAISE. Select with EXERCISE:<name>. SITUP/JUMPING_JACK use WRIST."
      : "WRIST exercises: SITUP | JUMPING_JACK. Select with EXERCISE:<name>. Other exercises use HIP.");
    return;
  }
  if (!strncmp(text, "EXERCISE:", 9)) {
    ExerciseType requestedExercise;
    if (!parseExercise(text + 9, requestedExercise)) {
      wirelessStreamer.message("ERROR: Unknown exercise. Send EXERCISES for names.");
      return;
    }
    if (!ExercisePresets::allowed(nodeRole, requestedExercise)) {
      wirelessStreamer.message(nodeRole == HIP
        ? "ERROR: This exercise uses WRIST. Select it and send ACTIVATE in the ExerciseWrist monitor."
        : "ERROR: This exercise uses HIP. Select it and send ACTIVATE in the ExerciseHip monitor.");
      return;
    }
    if (requestedExercise != activeExercise) {
      activeExercise = requestedExercise;
      nodeSession.selectExerciseMode(ExercisePresets::defaultMode(activeExercise), now);
      wirelessStreamer.message(nodeSession.localActive()
        ? "[Exercise changed; waiting for comms rep count with a new ticket]"
        : "[Exercise selected; still inactive. Send ACTIVATE to start]");
    }
    reportExercise();
    return;
  }
  if (!strcmp(text, "STANDBY") || !strcmp(text, "DEACTIVATE")) {
    nodeSession.standby();
    return;
  }

  if (!strcmp(text, "ACTIVATE")) {
    if (!NodeRadio::ready()) {
      wirelessStreamer.message("ERROR: ESP-NOW unavailable. Configure peer MACs and check USB boot messages.");
      return;
    }
    nodeSession.activate(now);
    return;
  }
  if (!strncmp(text, "SOURCE:", 7)) {
    wirelessStreamer.message("Use ACTIVATE in the desired node's own monitor; it will put the other node in standby.");
    return;
  }

  if (!strcmp(text, "RESET")) {
    if (nodeSession.waiting()) nodeSession.standby();
    restartSession(now);
    wirelessStreamer.message(nodeSession.localActive()
      ? "[Session reset; waiting for comms rep count with a new ticket]"
      : "[Session reset; still inactive. Send ACTIVATE to start]");
    return;
  }

  const char* mode = text;
  if (!strncmp(mode, "MODE:", 5)) {
    mode += 5;
    while (isspace((unsigned char)*mode)) {
      ++mode;
    }
  }

  CountingMode requested;
  if (!strcmp(mode, "PAUSE")) {
    requested = PAUSE_MODE;
  } else if (!strcmp(mode, "CONTINUOUS")) {
    requested = CONTINUOUS_MODE;
  } else {
    wirelessStreamer.message("ERROR: Unknown command. Send HELP for commands.");
    return;
  }

  nodeSession.setMode(requested, now);
}

TelemetryData makeTelemetry() {
  TelemetryData data = {};

  data.uptimeMs = millis();
  data.reps = commsReps.total();
  data.calibrationReps = 0;
  data.calibrationTarget = 0;

  data.repState = commsReps.received() ? COMMS_COUNTING : COMMS_WAITING;
  data.mode = (int)activeMode;
  if (!nodeSession.localActive()) data.mode = (int)nodeSession.selectedMode();
  data.exercise = (uint8_t)activeExercise;
  data.gyroAxis = -1;

  data.buzzer = buzzerFeedback.on();
  data.fallAlert = buzzerFeedback.fallActive();
  data.source = nodeRole;

  return data;
}

void begin(NodeRole role) {
  nodeRole = role;
  buzzerFeedback.begin();
  sensorHistory.clear(); // Boot only; counter resets must preserve sensor history.
  genericPeakDetector.reset();
  genericPeakPending = false;
  activeExercise = nodeRole == HIP ? SQUAT : JUMPING_JACK;
  activeMode = ExercisePresets::defaultMode(activeExercise);
  usbConsole.begin(115200);

  NodePlatform::outputPin(GeneralConfig::LED_PIN);
  setLed(false);

  if (nodeRole == HIP) {
    NodePlatform::outputPin(FallConfig::BUZZER_PIN);
    writePin(FallConfig::BUZZER_PIN, 0);
  }

  delayMs(1000);
  usbConsole.println(nodeRole == HIP ? "[BOOT] Hip restarting; standby until ACTIVATE."
                                : "[BOOT] Wrist restarting; standby until ACTIVATE.");

  // Print the STA MAC even during initial provisioning (before MPU checks).
  if (!NodeRadio::begin(nodeRole)) usbConsole.println("[ESP-NOW unavailable; activation will be refused]");

  if (!mpu.begin()) {
    usbConsole.println("ERROR: MPU6050 not found. Check the sensor wiring.");

    while (true) {
      setLed(true);
      delayMs(200);
      setLed(false);
      delayMs(200);
    }
  }

  const char* bluetoothName = nodeRole == HIP
    ? NodeConfig::HIP_BLUETOOTH_NAME : NodeConfig::WRIST_BLUETOOTH_NAME;
  wirelessStarted = wirelessStreamer.begin(bluetoothName);
  if (wirelessStarted) usbConsole.printf("[Bluetooth: %s | Ready]\n", bluetoothName);

  if (!wirelessStarted) {
    usbConsole.println("WARNING: Bluetooth unavailable. USB output is still active.");
  }

  const unsigned long now = millis();

  const uint32_t boot = esp_random(); // Generate after radio initialization, as for NodeSession.
  streamControl.begin(nodeRole, boot);
  commsReps.begin(boot);
  commsActivity.begin(boot);
  nodeSession.begin(nodeRole, boot, activeMode);
  restartSession(now);
  fallDetector.begin(now);

  wirelessStreamer.message(commandHelp);
  wirelessStreamer.message(repCommandHelp);
  wirelessStreamer.message(activityCommandHelp);
  wirelessStreamer.message(streamCommandHelp);
  wirelessStreamer.message(coordinationHelp);
  reportStreamStatus();
  nodeSession.status();
  reportExercise();
  reportActivity();

  lastSampleTime = micros();
  lastUsbOutputTime = now;
}

void update() {
  unsigned long now = millis();

  updateOutputTimers(now);

  char command[128];
  if (wirelessStreamer.pollCommand(command, sizeof(command))) {
    processCommand(command, millis());
  }

  LinkPacket packet;
  // Bound receive work so a busy link cannot starve sensor sampling.
  for (int i = 0; i < 12 && NodeRadio::receive(packet); ++i) {
    nodeSession.receive(packet, millis());
  }
  now = millis();
  nodeSession.update(now);
  NodeRadio::update();
  const bool restartRequested = nodeSession.consumeRestart();
  if (restartRequested || activeMode != nodeSession.mode()) {
    activeMode = nodeSession.mode();
    restartSession(now);
  }
  // Legacy peer LED patterns must not override the comms activity state.
  if (!nodeSession.localActive()) {
    buzzerFeedback.cancelReps();
    writeBuzzer();
  }

  const unsigned long sampleTime = micros();

  if (
    sampleTime - lastSampleTime <
    GeneralConfig::SAMPLE_INTERVAL_US
  ) {
    serviceWindowStream();
    return;
  }

  lastSampleTime = sampleTime;

  float acceleration[3];
  float gyro[3];
  const unsigned long readStarted = millis();
  if (!mpu.read(acceleration, gyro)) {
    // Do not feed invalid or stale samples into the motion/fall detectors.
    static unsigned long lastSensorError = 0;
    if (millis() - lastSensorError >= 1000) {
      usbConsole.println("[MPU6050 read failed; check wiring]");
      lastSensorError = millis();
    }
    serviceWindowStream();
    return;
  }

  now = millis();
  SensorSample sample = {};
  sample.timestampMs = (uint32_t)readStarted;
  memcpy(sample.acceleration, acceleration, sizeof(acceleration));
  memcpy(sample.gyro, gyro, sizeof(gyro));
  sensorHistory.push(sample); // Runs on both boards, including standby/activation failure.
  detectGenericPeak(sample); // Independent of exercise, activation and counting mode.
  serviceWindowStream();
  if (now - readStarted >= 100) {
    usbConsole.printf("[MPU6050 read blocked %lu ms]\n", now - readStarted);
  }

  const float rawMagnitude = sqrtf(
    acceleration[0] * acceleration[0] +
    acceleration[1] * acceleration[1] +
    acceleration[2] * acceleration[2]
  );

  if (nodeRole == HIP) fallDetector.update(rawMagnitude, now);

  if (nodeRole == HIP && fallDetector.consumeFallEvent()) {
    buzzerFeedback.fall((uint32_t)now);
    writeBuzzer();
  }

  const TelemetryData local = makeTelemetry();
  const TelemetryData data = nodeSession.output(local);

  if (now - lastUsbOutputTime >= GeneralConfig::OUTPUT_INTERVAL_MS) {
    lastUsbOutputTime = now;
    printTelemetry(usbConsole, data);
  }

  // Bluetooth output is queued without blocking sensor sampling.
  if (wirelessStarted) {
    wirelessStreamer.update(data, now);
    if (wirelessStreamer.consumeConnectionEvent()) {
      nodeSession.status();
      reportExercise();
      reportCommsReps();
      reportActivity();
      if (!NodeRadio::ready()) wirelessStreamer.message("[ESP-NOW unavailable; configure peer MACs. Send STATUS for details.]");
    }
  }
}

} // namespace ExerciseNode
