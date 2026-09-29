#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "ContinuousCounter.h"
#include "FallDetector.h"
#include "WirelessStreamer.h"

Adafruit_MPU6050 mpu;

ExerciseSettings genericSettings = getGenericSettings();

RepCounter repCounter(genericSettings);
ContinuousCounter continuousCounter;

FallDetector fallDetector;
WirelessStreamer wirelessStreamer;

bool wirelessStarted = false;

CountingMode activeMode = ModeConfig::REQUESTED_MODE;
bool countingStarted = false;

// Exclude calibration cycles from the displayed count.
int repetitionOffset = 0;

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

// LED output.
bool ledActive = false;
unsigned long ledStartTime = 0;

bool calibrationLedOffPulse = false;
unsigned long calibrationLedOffStartTime = 0;

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

  return countingStarted ? 2 : 1;
}

int calibrationProgress() {
  if (sessionState() == 0) {
    return 0;
  }

  return activeMode == PAUSE_MODE
    ? repCounter.calibrationCount()
    : continuousCounter.calibrationCount();
}

int reportedRepetitions() {
  if (!countingStarted) {
    return 0;
  }

  if (activeMode == PAUSE_MODE) {
    return repCounter.repetitionCount() - repetitionOffset;
  }

  if (activeMode == CONTINUOUS_MODE) {
    return continuousCounter.repetitionCount() - repetitionOffset;
  }

  return 0;
}

void updateOutputTimers(unsigned long now) {
  if (
    calibrationLedOffPulse &&
    now - calibrationLedOffStartTime >=
      GeneralConfig::CALIBRATION_LED_OFF_MS
  ) {
    calibrationLedOffPulse = false;

    if (sessionState() == 1) {
      setLed(true);
    }
  }

  if (
    ledActive &&
    now - ledStartTime >= GeneralConfig::REP_LED_DURATION_MS
  ) {
    ledActive = false;
    setLed(false);
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
  repetitionOffset = 0;
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

  ledActive = false;
  calibrationLedOffPulse = false;
  ledStartTime = now;
  calibrationLedOffStartTime = now;
  setLed(false);

  lastSampleTime = micros();
  lastUsbOutputTime = now - GeneralConfig::OUTPUT_INTERVAL_MS;
  // Reset only the exercise session. Bluetooth and fall monitoring keep running.
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
    wirelessStreamer.message("Commands: RESET | MODE:PAUSE | MODE:CONTINUOUS. Select Newline, then Send.");
    return;
  }
  if (!strcmp(text, "#OVERFLOW")) {
    wirelessStreamer.message("ERROR: Command too long. Use RESET, MODE:PAUSE or MODE:CONTINUOUS.");
    return;
  }

  if (!strcmp(text, "RESET")) {
    restartSession(now);
    wirelessStreamer.message(activeMode == PAUSE_MODE
      ? "OK: RESET | Mode: PAUSE | Reps cleared. Move into position, then hold still."
      : "OK: RESET | Mode: CONTINUOUS | Reps cleared. Move into position, then hold still.");
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
    wirelessStreamer.message("ERROR: Unknown command. Use RESET, MODE:PAUSE or MODE:CONTINUOUS.");
    return;
  }

  activeMode = requested;
  restartSession(now);
  wirelessStreamer.message(activeMode == PAUSE_MODE
    ? "OK: Mode: PAUSE | Reps cleared. Move into position, then hold still."
    : "OK: Mode: CONTINUOUS | Reps cleared. Move into position, then hold still.");
}

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

    continuousCounter.begin(
      gyroBias,
      now,
      false
    );

    calibrationLedOffPulse = false;
    ledActive = false;
    setLed(true);
  }
}

void startCounting() {
  countingStarted = true;

  if (activeMode == PAUSE_MODE) {
    repetitionOffset = repCounter.repetitionCount();
  } else {
    repetitionOffset = continuousCounter.repetitionCount();
  }

  calibrationLedOffPulse = false;
  ledActive = false;
  setLed(false);
}

void reportCalibrationResult(CalibrationResult result) {
  if (result == CAL_NO_RESULT) {
    return;
  }

  const char* reason = "";
  switch (result) {
    case CAL_TOO_SHORT: reason = "REJECTED: movement too short"; break;
    case CAL_TOO_LONG: reason = "REJECTED: timeout; no completed rep with a pause"; break;
    case CAL_TOO_WEAK: reason = "REJECTED: movement peak too weak"; break;
    case CAL_MISMATCH: reason = "REJECTED PAIR: peaks or durations differ; keeping latest rep"; break;
    case CAL_ACCEPTED: reason = "First candidate accepted; repeat at the same pace"; break;
    case CAL_FINISHED: reason = "Two matching candidates accepted; counting starts at zero"; break;
    default: return;
  }

  char message[192];
  snprintf(message, sizeof(message), "CAL: %s | Peak: %.2f m/s2 | Time: %.2f s",
    reason, repCounter.lastCompletedPeak(),
    repCounter.lastCompletedDuration() / 1000.0f);
  wirelessStreamer.message(message);

  if (result == CAL_FINISHED) {
    snprintf(message, sizeof(message),
      "PAUSE limits: Start > %.2f | Peak >= %.2f m/s2 | Time: %.2f-%.2f s (includes end pause)",
      repCounter.highThreshold(), repCounter.requiredRepPeak(),
      repCounter.minimumRepDuration() / 1000.0f,
      repCounter.maximumRepDuration() / 1000.0f);
    wirelessStreamer.message(message);
  }
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
    reportCalibrationResult(repCounter.consumeCalibrationResult());
    reportRepResult(repCounter.consumeRepResult());
  }

  if (activeMode == CONTINUOUS_MODE) {
    continuousCounter.update(gyro, stationary, now);
  }

  // Always consume each event separately.
  const bool pauseCalibrationEvent =
    repCounter.consumeCalibrationEvent();

  const bool continuousCalibrationEvent =
    continuousCounter.consumeCalibrationEvent();

  const bool pauseRepEvent =
    repCounter.consumeRepEvent();

  const bool continuousRepEvent =
    continuousCounter.consumeRepEvent();

  if (!countingStarted) {
    const bool pauseReady =
      repCounter.state() == REP_COUNTING;

    const bool continuousReady =
      continuousCounter.calibrated();

    if (
      (activeMode == PAUSE_MODE && pauseReady) ||
      (activeMode == CONTINUOUS_MODE && continuousReady)
    ) {
      startCounting();
      return;
    }

    if (pauseCalibrationEvent || continuousCalibrationEvent) {
      calibrationLedOffPulse = true;
      calibrationLedOffStartTime = now;
      setLed(false);
    }

    return;
  }

  const bool countedRep =
    (activeMode == PAUSE_MODE && pauseRepEvent) ||
    (activeMode == CONTINUOUS_MODE && continuousRepEvent);

  if (countedRep) {
    ledActive = true;
    ledStartTime = now;
    setLed(true);
  }
}

TelemetryData makeTelemetry() {
  TelemetryData data = {};

  data.uptimeMs = millis();
  data.reps = reportedRepetitions();
  data.calibrationReps = calibrationProgress();
  data.calibrationTarget = activeMode == PAUSE_MODE
    ? GeneralConfig::REQUIRED_STABLE_REPS
    : 2;

  data.repState = sessionState();
  data.mode = (int)activeMode;
  data.gyroAxis = continuousCounter.axis();
  data.pauseMotion = repCounter.motionSignal();
  data.pausePeak = repCounter.currentMovementPeak();

  data.positioningSeen = positioningMovementSeen;
  data.buzzer = buzzerActive;

  return data;
}

void setup() {
  Serial.begin(115200);

  pinMode(GeneralConfig::LED_PIN, OUTPUT);
  setLed(false);

  pinMode(FallConfig::BUZZER_PIN, OUTPUT);
  digitalWrite(FallConfig::BUZZER_PIN, LOW);

  delay(1000);
  Serial.println("[BOOT] ExerciseNode restarting; preparing baseline.");

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

  wirelessStarted = wirelessStreamer.begin("ExerciseNode");

  if (!wirelessStarted) {
    Serial.println("WARNING: Bluetooth unavailable. USB output is still active.");
  }

  const unsigned long now = millis();

  restartSession(now);
  fallDetector.begin(now);

  wirelessStreamer.message("Commands: RESET | MODE:PAUSE | MODE:CONTINUOUS. Select Newline, then Send.");

  lastSampleTime = micros();
  lastUsbOutputTime = now;
}

void loop() {
  unsigned long now = millis();

  updateOutputTimers(now);

  char command[32];
  if (wirelessStreamer.pollCommand(command, sizeof(command))) {
    processCommand(command, millis());
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

  fallDetector.update(rawMagnitude, now);

  if (fallDetector.consumeFallEvent()) {
    digitalWrite(FallConfig::BUZZER_PIN, HIGH);
    buzzerActive = true;
    buzzerStartTime = now;
  }

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

  const TelemetryData data = makeTelemetry();

  if (now - lastUsbOutputTime >= GeneralConfig::OUTPUT_INTERVAL_MS) {
    lastUsbOutputTime = now;
    printTelemetry(Serial, data);
  }

  // Show USB status before attempting a potentially blocking Bluetooth write.
  if (wirelessStarted) {
    wirelessStreamer.update(data, now);
  }
}
