#include "Mpu6050.h"
#include "NodePlatform.h"
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <esp_random.h>

#include "ExerciseNode.h"
#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "ContinuousCounter.h"
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
// Shared diagnostic scratch: keep full sensor windows off the ESP-IDF task stack.
ScheduledWindow windowScratch;

ExerciseSettings genericSettings = getGenericSettings();

RepCounter repCounter(genericSettings);
ContinuousCounter continuousCounter;
ExerciseType activeExercise = SQUAT;

FallDetector fallDetector;
WirelessStreamer wirelessStreamer;

void linkMessage(const char* text) { wirelessStreamer.message(text); }
NodeSession nodeSession(NodeRadio::send, linkMessage);
const char* commandHelp = "Commands: ACTIVATE | STANDBY | RESET | EXERCISE:<name> | EXERCISES | MODE:PAUSE | MODE:CONTINUOUS | BUFFER | WINDOW[:t0_ms] | STREAM | STATUS | HELP. Select Newline, then Send.";
const char* streamCommandHelp = "Windows: STREAM | STREAM:START[:t0_ms] | STREAM:STOP | STREAM:ARM | STREAM:NEXT. Local preparation only; no comms transmission.";

bool wirelessStarted = false;

CountingMode activeMode = PAUSE_MODE;
bool countingStarted = false;

unsigned long lastSampleTime = 0;
unsigned long lastUsbOutputTime = 0;

float filteredMagnitude = 0.0f;
bool filterInitialised = false;

// Buzzer output.
bool buzzerActive = false;
unsigned long buzzerStartTime = 0;

void setLed(bool on) {
  writePin(
    GeneralConfig::LED_PIN,
    on ? GeneralConfig::LED_ON : GeneralConfig::LED_OFF
  );
}

int sessionState() {
  return countingStarted &&
    (activeMode == PAUSE_MODE || continuousCounter.axis() >= 0) ? 2 : 1;
}

int reportedRepetitions() {
  if (!countingStarted) {
    return 0;
  }

  if (activeMode == PAUSE_MODE) {
    return repCounter.repetitionCount();
  }

  if (activeMode == CONTINUOUS_MODE) {
    return continuousCounter.repetitionCount();
  }

  return 0;
}

void updateOutputTimers(unsigned long now) {
  if (nodeSession.localActive() && countingStarted) {
    setLed(nodeSession.feedbackLedOn(now));
  }

  if (
    buzzerActive &&
    now - buzzerStartTime >= FallConfig::BUZZER_DURATION_MS
  ) {
    writePin(FallConfig::BUZZER_PIN, 0);
    buzzerActive = false;
  }
}

void startCounting(unsigned long now);

void restartSession(unsigned long now) {
  countingStarted = false;
  filteredMagnitude = 0.0f;
  filterInitialised = false;
  nodeSession.clearLedFeedback(now);
  setLed(false);

  if (!repCounter.beginPreset(now, ExercisePresets::pause(activeExercise))) {
    nodeSession.standby();
    wirelessStreamer.message("ERROR: Invalid exercise preset; counting remains inactive.");
    return;
  }
  continuousCounter.beginPreset(GeneralConfig::GYRO_BIAS_RAD_S, now,
                                ExercisePresets::continuous(activeExercise));

  // Standby may prepare presets, but only acknowledged activation starts counting.
  if (nodeSession.localActive()) {
    wirelessStreamer.message(activeMode == PAUSE_MODE
      ? "[Preset loaded; counting starts at zero without standstill. Pause briefly between reps.]"
      : "[Gyro preset loaded; no standstill. Keep moving to find the axis; initial movement may not count.]");
    startCounting(now);
  }

  lastUsbOutputTime = now - GeneralConfig::OUTPUT_INTERVAL_MS;
  // Reset only counting. Sensor history and sampling cadence keep running.
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

void reportStreamStatus() {
  const WindowStreamStatus status = windowScheduler.status();
  const char* state = status.state == WindowStreamState::RUNNING ? "RUNNING"
    : status.state == WindowStreamState::ARMED ? "ARMED" : "STOPPED";
  char line[216];
  snprintf(line, sizeof(line),
    "[STREAM | %s | %s | session=%lu | t0_ms=%lu | next_end_ms=%lu | queue=%u | dropped=%lu | skipped=%lu | local_only]",
    nodeRole == HIP ? "HIP" : "WRIST", state, (unsigned long)status.session,
    (unsigned long)status.triggerMs, (unsigned long)status.nextEndMs, (unsigned int)status.queued,
    (unsigned long)status.dropped, (unsigned long)status.skipped);
  wirelessStreamer.message(line);
}

void reportPreparedWindow() {
  const WindowPreparation& prepared = windowScheduler.lastPreparation();
  const WindowStreamStatus status = windowScheduler.status();
  const SensorWindowResult& window = prepared.info.window;
  char line[216];
  snprintf(line, sizeof(line),
    "[FRAME | %s | session=%lu | seq=%lu | %s | start_ms=%lu | end_ms=%lu | samples=%u | queue=%u | dropped=%lu | skipped=%lu]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)prepared.info.session,
    (unsigned long)prepared.info.sequence, prepared.queued ? sensorWindowStatusName(window.status) : "QUEUE_FULL",
    (unsigned long)window.startMs, (unsigned long)window.endMs, (unsigned int)window.count,
    (unsigned int)status.queued, (unsigned long)status.dropped, (unsigned long)status.skipped);
  wirelessStreamer.message(line);
}

bool startWindowStream(uint32_t t0) {
  if (!windowScheduler.start(t0, (uint32_t)millis(), sensorHistory)) return false;
  reportStreamStatus();
  reportPreparedWindow();
  return true;
}

void stopWindowStream() { windowScheduler.stop(); }
void armWindowStream() { windowScheduler.arm(); }

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
  // Freeze sequence zero before any diagnostic output can delay the main loop.
  const bool started = windowScheduler.status().state == WindowStreamState::ARMED &&
    windowScheduler.start(event.peakMs, (uint32_t)millis(), sensorHistory);
  // Diagnostic metadata only; no window payload, activation or peer trigger.
  char line[192];
  snprintf(line, sizeof(line),
    "[PEAK | %s | t0_ms=%lu | confirmed_ms=%lu | accel=%.2f m/s2 | gyro=%.2f rad/s]",
    nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)event.peakMs,
    (unsigned long)event.detectedMs, event.accelerationMps2, event.gyroRadS);
  wirelessStreamer.message(line);
  if (started) {
    reportStreamStatus();
    reportPreparedWindow();
  }
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
  const SensorWindowResult result = copySensorWindow(endMs, windowScratch.samples, WindowSchedulerConfig::SAMPLE_CAPACITY);
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

void reportExercise() {
  char message[160];
  const CountingMode selectedMode = nodeSession.selectedMode();
  snprintf(message, sizeof(message), "[Exercise: %s | %s | Preset: %s | No standstill or calibration reps]",
    exerciseName(activeExercise), selectedMode == PAUSE_MODE ? "PAUSE" : "CONTINUOUS",
    selectedMode == PAUSE_MODE ? ExercisePresets::get(activeExercise).description : "mock gyro limits");
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
    wirelessStreamer.message(streamCommandHelp);
    return;
  }
  if (!strcmp(text, "STATUS")) {
    nodeSession.status();
    reportExercise();
    reportStreamStatus();
    wirelessStreamer.message(NodeRadio::ready() ? "[ESP-NOW configured and ready]"
      : "[ESP-NOW unavailable; check peer MAC configuration and USB boot messages]");
    return;
  }
  if (!strcmp(text, "STREAM")) {
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
    if (!takeSensorWindow(windowScratch)) {
      wirelessStreamer.message("[FRAME TAKEN | EMPTY]");
      return;
    }
    char line[192];
    const ScheduledWindowInfo& info = windowScratch.info;
    snprintf(line, sizeof(line),
      "[FRAME TAKEN | %s | session=%lu | seq=%lu | %s | start_ms=%lu | end_ms=%lu | samples=%u]",
      nodeRole == HIP ? "HIP" : "WRIST", (unsigned long)info.session, (unsigned long)info.sequence,
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
        ? "[Exercise changed; restarting with its preset, no standstill required]"
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
      ? "[Session reset; preset reloaded, no standstill required]"
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

void startCounting(unsigned long now) {
  countingStarted = true;

  nodeSession.showLedFeedback(LedFeedback::READY, now);
  setLed(nodeSession.feedbackLedOn(now));
}

void reportRepResult(RepResult result) {
  if (result == REP_NO_RESULT) {
    return;
  }

  const char* reason = "";
  switch (result) {
    case REP_TOO_SHORT: reason = "movement too short"; break;
    case REP_TOO_LONG: reason = "timeout before a completed rep with a pause"; break;
    case REP_TOO_WEAK:
      reason = repCounter.lastCompletedDuration() < repCounter.minimumRepDuration()
        ? "peak too weak AND movement too short"
        : "movement peak too weak";
      break;
    default: return;
  }

  char message[192];
  snprintf(message, sizeof(message),
    "REP skipped: %s | Peak: %.2f m/s2 / allowed >= %.2f m/s2 | Time: %.2f s / allowed %.2f-%.2f s",
    reason, repCounter.lastCompletedPeak(), repCounter.requiredRepPeak(),
    repCounter.lastCompletedDuration() / 1000.0f,
    repCounter.minimumRepDuration() / 1000.0f,
    repCounter.maximumRepDuration() / 1000.0f);
  wirelessStreamer.message(message);
}

void processMovement(
  const float gyro[3],
  unsigned long now
) {
  float correctedGyroSquared = 0.0f;

  for (int i = 0; i < 3; ++i) {
    const float corrected = gyro[i] - GeneralConfig::GYRO_BIAS_RAD_S[i];
    correctedGyroSquared += corrected * corrected;
  }

  const float correctedGyroMagnitude =
    sqrtf(correctedGyroSquared);

  const float accelerationMotion = fabsf(
    filteredMagnitude - repCounter.baselineMagnitude()
  );

  const bool stationary =
    accelerationMotion < genericSettings.calibrationEndGate &&
    correctedGyroMagnitude < ModeConfig::QUIET_GYRO_LIMIT;

  if (activeMode == PAUSE_MODE) {
    repCounter.update(filteredMagnitude, now);
    reportRepResult(repCounter.consumeRepResult());
  }

  if (activeMode == CONTINUOUS_MODE) {
    continuousCounter.update(gyro, stationary, now);
  }

  const bool pauseRepEvent =
    repCounter.consumeRepEvent();

  const bool continuousRepEvent =
    continuousCounter.consumeRepEvent();

  const bool countedRep =
    (activeMode == PAUSE_MODE && pauseRepEvent) ||
    (activeMode == CONTINUOUS_MODE && continuousRepEvent);

  if (countedRep) {
    nodeSession.showLedFeedback(LedFeedback::REP, now);
    setLed(true);
  }
}

TelemetryData makeTelemetry() {
  TelemetryData data = {};

  data.uptimeMs = millis();
  data.reps = reportedRepetitions();
  data.calibrationReps = 0;
  data.calibrationTarget = 0;

  data.repState = sessionState();
  data.mode = (int)activeMode;
  if (!nodeSession.localActive()) data.mode = (int)nodeSession.selectedMode();
  data.exercise = (uint8_t)activeExercise;
  data.gyroAxis = continuousCounter.axis();
  data.pauseMotion = repCounter.motionSignal();
  data.pausePeak = repCounter.currentMovementPeak();

  data.buzzer = buzzerActive;
  data.source = nodeRole;

  return data;
}

void begin(NodeRole role) {
  nodeRole = role;
  sensorHistory.clear(); // Boot only; counter resets must preserve sensor history.
  genericPeakDetector.reset();
  genericPeakPending = false;
  windowScheduler.reset();
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

  nodeSession.begin(nodeRole, esp_random(), activeMode);
  restartSession(now);
  fallDetector.begin(now);

  wirelessStreamer.message(commandHelp);
  wirelessStreamer.message(streamCommandHelp);
  reportStreamStatus();
  nodeSession.status();
  reportExercise();

  lastSampleTime = micros();
  lastUsbOutputTime = now;
}

void update() {
  unsigned long now = millis();

  updateOutputTimers(now);

  char command[32];
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
  if (!nodeSession.localActive()) {
    setLed(nodeSession.feedbackLedOn(now));
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

  if (fallDetector.consumeFallEvent()) {
    writePin(FallConfig::BUZZER_PIN, 1);
    buzzerActive = true;
    buzzerStartTime = now;
  }

  if (nodeSession.localActive() && countingStarted) {
    if (!filterInitialised) {
      filteredMagnitude = rawMagnitude;
      filterInitialised = true;
    } else {
      filteredMagnitude =
        GeneralConfig::FILTER_ALPHA * rawMagnitude +
        (1.0f - GeneralConfig::FILTER_ALPHA) *
          filteredMagnitude;
    }

    processMovement(gyro, now);
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
      if (!NodeRadio::ready()) wirelessStreamer.message("[ESP-NOW unavailable; configure peer MACs. Send STATUS for details.]");
    }
  }
}

} // namespace ExerciseNode
