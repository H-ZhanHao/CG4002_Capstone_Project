#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "FallDetector.h"
#include "ExerciseRecognition.h"
#include "WirelessStreamer.h"

// ======================================================
// OBJECTS
// No exercise selection is used to construct the counter.
// ======================================================

Adafruit_MPU6050 mpu;

RepCounter repCounter(getGenericSettings());

FallDetector fallDetector;
ExerciseRecognition exerciseRecognition;
WirelessStreamer wirelessStreamer;

bool wirelessStarted = false;

// ======================================================
// SAMPLING AND FILTERING
// ======================================================

unsigned long lastSampleTime = 0;
unsigned long lastUsbOutputTime = 0;

float filteredMagnitude = 0.0f;
bool filterInitialised = false;

// ======================================================
// BASELINE PREPARATION
// ======================================================

bool positioningMovementSeen = false;

bool previousMagnitudeAvailable = false;
float previousFilteredMagnitude = 0.0f;

// ======================================================
// LED STATE
// ======================================================

bool ledActive = false;
unsigned long ledStartTime = 0;

bool calibrationLedOffPulse = false;
unsigned long calibrationLedOffStartTime = 0;

// ======================================================
// BUZZER STATE
// ======================================================

bool buzzerActive = false;
unsigned long buzzerStartTime = 0;

// ======================================================
// LED HELPER
// ======================================================

void setLed(bool on) {
  digitalWrite(
    GeneralConfig::LED_PIN,
    on ? GeneralConfig::LED_ON : GeneralConfig::LED_OFF
  );
}

// ======================================================
// NON-BLOCKING OUTPUT TIMERS
// ======================================================

void updateOutputTimers(unsigned long currentTime) {
  if (
    calibrationLedOffPulse &&
    currentTime - calibrationLedOffStartTime >=
      GeneralConfig::CALIBRATION_LED_OFF_MS
  ) {
    calibrationLedOffPulse = false;

    if (repCounter.state() == MOVEMENT_CALIBRATION) {
      setLed(true);
    }
  }

  if (
    ledActive &&
    currentTime - ledStartTime >=
      GeneralConfig::REP_LED_DURATION_MS
  ) {
    ledActive = false;
    setLed(false);
  }

  if (
    buzzerActive &&
    currentTime - buzzerStartTime >=
      FallConfig::BUZZER_DURATION_MS
  ) {
    digitalWrite(FallConfig::BUZZER_PIN, LOW);
    buzzerActive = false;
  }
}

// ======================================================
// BASELINE PREPARATION AND REP COUNTING
// ======================================================

void updateRepCounting(
  float gyroMagnitude,
  unsigned long currentTime
) {
  const bool hadPreviousMagnitude =
    previousMagnitudeAvailable;

  float accelerationChange = 0.0f;

  if (hadPreviousMagnitude) {
    accelerationChange = fabsf(
      filteredMagnitude - previousFilteredMagnitude
    );
  }

  previousFilteredMagnitude = filteredMagnitude;
  previousMagnitudeAvailable = true;

  if (repCounter.state() == BASELINE_CALIBRATION) {
    const bool positioningMovement =
      gyroMagnitude >
      GeneralConfig::POSITIONING_MOVEMENT_GYRO;

    // Wait until positioning movement has occurred.
    if (!positioningMovementSeen) {
      if (positioningMovement) {
        positioningMovementSeen = true;
      }

      repCounter.begin(currentTime);
      return;
    }

    const bool stationary =
      hadPreviousMagnitude &&
      gyroMagnitude <
        GeneralConfig::STATIONARY_MAX_GYRO &&
      accelerationChange <
        GeneralConfig::STATIONARY_MAX_ACCEL_CHANGE;

    if (!stationary) {
      // Restart the stationary baseline window.
      repCounter.begin(currentTime);
      return;
    }

    repCounter.update(
      filteredMagnitude,
      currentTime
    );

    if (repCounter.state() == MOVEMENT_CALIBRATION) {
      calibrationLedOffPulse = false;
      ledActive = false;
      setLed(true);
    }

    return;
  }

  const CounterState previousState = repCounter.state();

  repCounter.update(
    filteredMagnitude,
    currentTime
  );

  const bool calibrationEvent =
    repCounter.consumeCalibrationEvent();

  if (
    previousState == MOVEMENT_CALIBRATION &&
    repCounter.state() == REP_COUNTING
  ) {
    // Two matching candidates have completed calibration.
    calibrationLedOffPulse = false;
    ledActive = false;
    setLed(false);
  } else if (
    calibrationEvent &&
    repCounter.state() == MOVEMENT_CALIBRATION
  ) {
    // Brief OFF pulse for a valid calibration candidate.
    calibrationLedOffPulse = true;
    calibrationLedOffStartTime = currentTime;
    setLed(false);
  }

  if (repCounter.consumeRepEvent()) {
    ledActive = true;
    ledStartTime = currentTime;
    setLed(true);
  }
}

// ======================================================
// RECOGNITION LIFECYCLE
// The exercise label never changes counting settings.
// ======================================================

void updateRecognition(
  CounterState previousState,
  const float acceleration[3],
  const float gyroscope[3],
  unsigned long currentTime
) {
  const CounterState currentState = repCounter.state();

  if (
    previousState == BASELINE_CALIBRATION &&
    currentState == MOVEMENT_CALIBRATION
  ) {
    exerciseRecognition.beginCalibration(currentTime);
  }

  if (previousState == MOVEMENT_CALIBRATION) {
    // Includes the final sample that completes calibration.
    exerciseRecognition.update(
      acceleration,
      gyroscope,
      repCounter.calibrationCount(),
      currentTime
    );

    if (currentState == REP_COUNTING) {
      // Freeze whichever result exists, including UNKNOWN.
      // Rep counting never waits for the recognition feature.
      exerciseRecognition.endCalibration();
    }
  }
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);

  pinMode(GeneralConfig::LED_PIN, OUTPUT);
  setLed(false);

  pinMode(FallConfig::BUZZER_PIN, OUTPUT);
  digitalWrite(FallConfig::BUZZER_PIN, LOW);

  delay(1000);

  if (!mpu.begin()) {
    Serial.println("MPU6050_Error:0");

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

  wirelessStarted =
    wirelessStreamer.begin("ExerciseNode");

  if (!wirelessStarted) {
    Serial.println("Bluetooth_Error:0");
  }

  const unsigned long currentTime = millis();

  repCounter.begin(currentTime);
  fallDetector.begin(currentTime);

  lastSampleTime = micros();
  lastUsbOutputTime = currentTime;
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  unsigned long currentTime = millis();

  updateOutputTimers(currentTime);

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

  mpu.getEvent(
    &accelerationEvent,
    &gyroscopeEvent,
    &temperatureEvent
  );

  currentTime = millis();

  const float acceleration[3] = {
    accelerationEvent.acceleration.x,
    accelerationEvent.acceleration.y,
    accelerationEvent.acceleration.z
  };

  const float gyroscope[3] = {
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
    gyroscope[0] * gyroscope[0] +
    gyroscope[1] * gyroscope[1] +
    gyroscope[2] * gyroscope[2]
  );

  // ----------------------------------------------------
  // Fall detection
  // ----------------------------------------------------

  fallDetector.update(
    rawMagnitude,
    currentTime
  );

  if (fallDetector.consumeFallEvent()) {
    digitalWrite(FallConfig::BUZZER_PIN, HIGH);

    buzzerActive = true;
    buzzerStartTime = currentTime;
  }

  // ----------------------------------------------------
  // Acceleration-magnitude filtering
  // ----------------------------------------------------

  if (!filterInitialised) {
    filteredMagnitude = rawMagnitude;
    filterInitialised = true;
  } else {
    filteredMagnitude =
      GeneralConfig::FILTER_ALPHA * rawMagnitude +
      (1.0f - GeneralConfig::FILTER_ALPHA) *
        filteredMagnitude;
  }

  // ----------------------------------------------------
  // Local rep counting
  // ----------------------------------------------------

  const CounterState previousState = repCounter.state();

  updateRepCounting(
    gyroMagnitude,
    currentTime
  );

  // ----------------------------------------------------
  // Recognition during movement calibration only
  // ----------------------------------------------------

  updateRecognition(
    previousState,
    acceleration,
    gyroscope,
    currentTime
  );

  // ----------------------------------------------------
  // Bluetooth output
  // ----------------------------------------------------

  if (wirelessStarted) {
    wirelessStreamer.update(
      rawMagnitude,
      filteredMagnitude,
      repCounter,
      fallDetector,
      exerciseRecognition,
      buzzerActive,
      currentTime
    );
  }

  // ----------------------------------------------------
  // USB output
  // ----------------------------------------------------

  if (
    currentTime - lastUsbOutputTime >=
    GeneralConfig::OUTPUT_INTERVAL_MS
  ) {
    lastUsbOutputTime = currentTime;

    printTelemetry(
      Serial,
      rawMagnitude,
      filteredMagnitude,
      repCounter,
      fallDetector,
      exerciseRecognition,
      buzzerActive
    );
  }
}