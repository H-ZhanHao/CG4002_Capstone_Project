#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <esp_system.h>

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

NodeRole nodeRole = HIP;
Adafruit_MPU6050 mpu;

ExerciseSettings genericSettings = getGenericSettings();

RepCounter repCounter(genericSettings);
ContinuousCounter continuousCounter;
ExerciseType activeExercise = SQUAT;

FallDetector fallDetector;
WirelessStreamer wirelessStreamer;

void linkMessage(const char* text) { wirelessStreamer.message(text); }
NodeSession nodeSession(NodeRadio::send, linkMessage);
const char* commandHelp = "Commands: ACTIVATE | STANDBY | RESET | EXERCISE:<name> | EXERCISES | MODE:PAUSE | MODE:CONTINUOUS | STATUS | HELP. Select Newline, then Send.";

bool wirelessStarted = false;

CountingMode activeMode = PAUSE_MODE;
bool countingStarted = false;

unsigned long lastSampleTime = 0;
unsigned long lastUsbOutputTime = 0;

float filteredMagnitude = 0.0f;
bool filterInitialised = false;

bool positioningMovementSeen = false;
bool previousMagnitudeAvailable = false;
float previousFilteredMagnitude = 0.0f;

// Gyro bias measured during the stationary baseline.
float gyroBaselineSum[3] = {};
float gyroBias[3] = {};
unsigned int gyroBaselineSamples = 0;

// Buzzer output.
bool buzzerActive = false;
unsigned long buzzerStartTime = 0;

void setLed(bool on) {
  digitalWrite(
    GeneralConfig::LED_PIN,
    on ? GeneralConfig::LED_ON : GeneralConfig::LED_OFF
  );
}

int sessionState() {
  if (repCounter.state() == BASELINE_CALIBRATION) {
    return 0;
  }

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
    digitalWrite(FallConfig::BUZZER_PIN, LOW);
    buzzerActive = false;
  }
}

void restartBaseline(unsigned long now) {
  repCounter.begin(now);

  gyroBaselineSamples = 0;

  for (int i = 0; i < 3; ++i) {
    gyroBaselineSum[i] = 0.0f;
  }
}

void restartSession(unsigned long now) {
  countingStarted = false;
  filteredMagnitude = 0.0f;
  filterInitialised = false;
  positioningMovementSeen = false;
  previousMagnitudeAvailable = false;
  previousFilteredMagnitude = 0.0f;

  for (int i = 0; i < 3; ++i) {
    gyroBias[i] = 0.0f;
  }

  restartBaseline(now);
  continuousCounter.begin(gyroBias, now, false);

  nodeSession.clearLedFeedback(now);
  setLed(false);

  lastSampleTime = micros();
  lastUsbOutputTime = now - GeneralConfig::OUTPUT_INTERVAL_MS;
  // Reset only the exercise session. Bluetooth and fall monitoring keep running.
}

void reportExercise() {
  char message[160];
  const CountingMode selectedMode = nodeSession.selectedMode();
  snprintf(message, sizeof(message), "[Exercise: %s | %s | Preset: %s | No calibration reps]",
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
    return;
  }
  if (!strcmp(text, "STATUS")) {
    nodeSession.status();
    reportExercise();
    wirelessStreamer.message(NodeRadio::ready() ? "[ESP-NOW configured and ready]"
      : "[ESP-NOW unavailable; check peer MAC configuration and USB boot messages]");
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
        ? "[Exercise changed; move into position, then hold still for 3 seconds]"
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
      ? "[Session reset; move into position, then hold still for 3 seconds]"
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

void startCounting(unsigned long now);

void updateBaseline(
  const float gyro[3],
  float gyroMagnitude,
  unsigned long now
) {
  const bool hadPrevious = previousMagnitudeAvailable;

  float change = 0.0f;

  if (hadPrevious) {
    change = fabsf(
      filteredMagnitude - previousFilteredMagnitude
    );
  }

  previousFilteredMagnitude = filteredMagnitude;
  previousMagnitudeAvailable = true;

  if (!positioningMovementSeen) {
    if (
      gyroMagnitude >
      GeneralConfig::POSITIONING_MOVEMENT_GYRO
    ) {
      positioningMovementSeen = true;
    }

    restartBaseline(now);
    return;
  }

  const bool stationary =
    hadPrevious &&
    gyroMagnitude < GeneralConfig::STATIONARY_MAX_GYRO &&
    change < GeneralConfig::STATIONARY_MAX_ACCEL_CHANGE;

  if (!stationary) {
    restartBaseline(now);
    return;
  }

  ++gyroBaselineSamples;

  for (int i = 0; i < 3; ++i) {
    gyroBaselineSum[i] += gyro[i];
  }

  repCounter.update(filteredMagnitude, now);

  if (repCounter.state() == MOVEMENT_CALIBRATION) {
    for (int i = 0; i < 3; ++i) {
      gyroBias[i] =
        gyroBaselineSum[i] / gyroBaselineSamples;
    }

    if (activeMode == PAUSE_MODE) {
      if (!repCounter.loadPreset(ExercisePresets::pause(activeExercise))) return;
    } else {
      continuousCounter.beginPreset(gyroBias, now, ExercisePresets::continuous(activeExercise));
    }
    wirelessStreamer.message(activeMode == PAUSE_MODE
      ? "[Fixed preset loaded; counting starts at zero. Pause briefly between reps.]"
      : "[Mock gyro preset loaded; start moving to find the axis. Initial movement may not count.]");
    startCounting(now);
  }
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
    const float corrected = gyro[i] - gyroBias[i];
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

  data.positioningSeen = positioningMovementSeen;
  data.buzzer = buzzerActive;
  data.source = nodeRole;

  return data;
}

void begin(NodeRole role) {
  nodeRole = role;
  activeExercise = nodeRole == HIP ? SQUAT : JUMPING_JACK;
  activeMode = ExercisePresets::defaultMode(activeExercise);
  Serial.begin(115200);

  pinMode(GeneralConfig::LED_PIN, OUTPUT);
  setLed(false);

  if (nodeRole == HIP) {
    pinMode(FallConfig::BUZZER_PIN, OUTPUT);
    digitalWrite(FallConfig::BUZZER_PIN, LOW);
  }

  delay(1000);
  Serial.println(nodeRole == HIP ? "[BOOT] Hip restarting; standby until ACTIVATE."
                                : "[BOOT] Wrist restarting; standby until ACTIVATE.");

  // Print the STA MAC even during initial provisioning (before MPU checks).
  if (!NodeRadio::begin(nodeRole)) Serial.println("[ESP-NOW unavailable; activation will be refused]");

  if (!mpu.begin()) {
    Serial.println("ERROR: MPU6050 not found. Check the sensor wiring.");

    while (true) {
      setLed(true);
      delay(200);
      setLed(false);
      delay(200);
    }
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  const char* bluetoothName = nodeRole == HIP
    ? NodeConfig::HIP_BLUETOOTH_NAME : NodeConfig::WRIST_BLUETOOTH_NAME;
  wirelessStarted = wirelessStreamer.begin(bluetoothName);
  if (wirelessStarted) Serial.printf("[Bluetooth: %s | Ready]\n", bluetoothName);

  if (!wirelessStarted) {
    Serial.println("WARNING: Bluetooth unavailable. USB output is still active.");
  }

  const unsigned long now = millis();

  nodeSession.begin(nodeRole, esp_random(), activeMode);
  restartSession(now);
  fallDetector.begin(now);

  wirelessStreamer.message(commandHelp);
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
    return;
  }

  lastSampleTime = sampleTime;

  sensors_event_t accelerationEvent;
  sensors_event_t gyroscopeEvent;
  sensors_event_t temperatureEvent;

  const unsigned long readStarted = millis();
  mpu.getEvent(
    &accelerationEvent,
    &gyroscopeEvent,
    &temperatureEvent
  );

  now = millis();
  if (now - readStarted >= 100) {
    Serial.printf("[MPU6050 read blocked %lu ms]\n", now - readStarted);
  }

  const float acceleration[3] = {
    accelerationEvent.acceleration.x,
    accelerationEvent.acceleration.y,
    accelerationEvent.acceleration.z
  };

  const float gyro[3] = {
    gyroscopeEvent.gyro.x,
    gyroscopeEvent.gyro.y,
    gyroscopeEvent.gyro.z
  };

  const float rawMagnitude = sqrtf(
    acceleration[0] * acceleration[0] +
    acceleration[1] * acceleration[1] +
    acceleration[2] * acceleration[2]
  );

  const float gyroMagnitude = sqrtf(
    gyro[0] * gyro[0] +
    gyro[1] * gyro[1] +
    gyro[2] * gyro[2]
  );

  if (nodeRole == HIP) fallDetector.update(rawMagnitude, now);

  if (fallDetector.consumeFallEvent()) {
    digitalWrite(FallConfig::BUZZER_PIN, HIGH);
    buzzerActive = true;
    buzzerStartTime = now;
  }

  if (nodeSession.localActive()) {
    if (!filterInitialised) {
      filteredMagnitude = rawMagnitude;
      filterInitialised = true;
    } else {
      filteredMagnitude =
        GeneralConfig::FILTER_ALPHA * rawMagnitude +
        (1.0f - GeneralConfig::FILTER_ALPHA) *
          filteredMagnitude;
    }

    if (repCounter.state() == BASELINE_CALIBRATION) {
      updateBaseline(gyro, gyroMagnitude, now);
    } else {
      processMovement(gyro, now);
    }
  }

  const TelemetryData local = makeTelemetry();
  const TelemetryData data = nodeSession.output(local);

  if (now - lastUsbOutputTime >= GeneralConfig::OUTPUT_INTERVAL_MS) {
    lastUsbOutputTime = now;
    printTelemetry(Serial, data);
  }

  // Show USB status before attempting a potentially blocking Bluetooth write.
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
