#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "FallDetector.h"
#include "WirelessStreamer.h"

// ======================================================
// OBJECTS AND EXERCISE SELECTION
// ======================================================

Adafruit_MPU6050 mpu;

// Change to PUSH_UP when testing push-ups.
const ExerciseType SELECTED_EXERCISE = SQUAT;

ExerciseSettings exerciseSettings =
  getExerciseSettings(SELECTED_EXERCISE);

RepCounter repCounter(exerciseSettings);
FallDetector fallDetector;
WirelessStreamer wirelessStreamer;

bool wirelessStarted = false;

// ======================================================
// SAMPLING AND FILTERING
// ======================================================

unsigned long lastSampleTime = 0;

float filteredMagnitude = 0.0;
bool filterInitialised = false;

// ======================================================
// BASELINE PREPARATION
// ======================================================

bool positioningMovementSeen = false;

bool previousMagnitudeAvailable = false;
float previousFilteredMagnitude = 0.0;

// ======================================================
// LED STATE
// ======================================================

bool ledActive = false;
unsigned long ledStartTime = 0;

bool calibrationReadyBlinking = false;
bool calibrationBlinkLedOn = false;

int calibrationBlinksCompleted = 0;
unsigned long calibrationBlinkChangeTime = 0;

// ======================================================
// BUZZER STATE
// ======================================================

bool buzzerActive = false;
unsigned long buzzerStartTime = 0;

// ======================================================
// SERIAL OUTPUT
// ======================================================

void printOutput(float rawMagnitude) {
  Serial.print("Raw:");
  Serial.print(rawMagnitude, 3);

  Serial.print("\tFiltered:");
  Serial.print(filteredMagnitude, 3);

  Serial.print("\tMotion:");
  Serial.print(repCounter.motionSignal(), 3);

  Serial.print("\tHigh:");
  Serial.print(repCounter.highThreshold(), 3);

  Serial.print("\tLow:");
  Serial.print(repCounter.lowThreshold(), 3);

  Serial.print("\tRequiredPeak:");
  Serial.print(repCounter.requiredRepPeak(), 3);

  Serial.print("\tLastPeak:");
  Serial.print(repCounter.lastCompletedPeak(), 3);

  Serial.print("\tMinTimeS:");
  Serial.print(
    repCounter.minimumRepDuration() / 1000.0,
    2
  );

  Serial.print("\tMaxTimeS:");
  Serial.print(
    repCounter.maximumRepDuration() / 1000.0,
    2
  );

  Serial.print("\tLastTimeS:");
  Serial.print(
    repCounter.lastCompletedDuration() / 1000.0,
    2
  );

  Serial.print("\tReps:");
  Serial.print(repCounter.repetitionCount());

  Serial.print("\tCalibrationReps:");
  Serial.print(repCounter.calibrationCount());

  Serial.print("\tRepState:");
  Serial.print((int)repCounter.state());

  Serial.print("\tPositioningSeen:");
  Serial.print(positioningMovementSeen ? 1 : 0);

  Serial.print("\tReadyBlink:");
  Serial.print(calibrationReadyBlinking ? 1 : 0);

  Serial.print("\tFallState:");
  Serial.print((int)fallDetector.state());

  Serial.print("\tBuzzer:");
  Serial.print(buzzerActive ? 1 : 0);

  Serial.print("\tExercise:");
  Serial.println((int)SELECTED_EXERCISE);
}

// ======================================================
// CALIBRATION READY LED PATTERN
// ======================================================

void startCalibrationReadyBlink(
  unsigned long currentTime
) {
  calibrationReadyBlinking = true;
  calibrationBlinkLedOn = true;
  calibrationBlinksCompleted = 0;

  calibrationBlinkChangeTime = currentTime;

  // Cancel any normal rep flash.
  ledActive = false;

  digitalWrite(
    GeneralConfig::LED_PIN,
    GeneralConfig::LED_ON
  );
}

void updateCalibrationReadyBlink(
  unsigned long currentTime
) {
  if (!calibrationReadyBlinking) {
    return;
  }

  if (
    currentTime - calibrationBlinkChangeTime <
    GeneralConfig::CALIBRATION_READY_BLINK_INTERVAL_MS
  ) {
    return;
  }

  calibrationBlinkChangeTime = currentTime;

  if (calibrationBlinkLedOn) {
    digitalWrite(
      GeneralConfig::LED_PIN,
      GeneralConfig::LED_OFF
    );

    calibrationBlinkLedOn = false;
    calibrationBlinksCompleted++;

    if (
      calibrationBlinksCompleted >=
      GeneralConfig::CALIBRATION_READY_BLINKS
    ) {
      calibrationReadyBlinking = false;
    }
  } else {
    digitalWrite(
      GeneralConfig::LED_PIN,
      GeneralConfig::LED_ON
    );

    calibrationBlinkLedOn = true;
  }
}

// ======================================================
// NON-BLOCKING OUTPUT TIMERS
// ======================================================

void updateOutputTimers(unsigned long currentTime) {
  updateCalibrationReadyBlink(currentTime);

  if (
    !calibrationReadyBlinking &&
    ledActive &&
    currentTime - ledStartTime >=
      GeneralConfig::REP_LED_DURATION_MS
  ) {
    digitalWrite(
      GeneralConfig::LED_PIN,
      GeneralConfig::LED_OFF
    );

    ledActive = false;
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

  float accelerationChange = 0.0;

  if (hadPreviousMagnitude) {
    accelerationChange = fabs(
      filteredMagnitude - previousFilteredMagnitude
    );
  }

  previousFilteredMagnitude = filteredMagnitude;
  previousMagnitudeAvailable = true;

  // Do not accept calibration reps during the LED pattern.
  if (calibrationReadyBlinking) {
    return;
  }

  if (repCounter.state() == BASELINE_CALIBRATION) {
    const bool positioningMovement =
      gyroMagnitude >
      GeneralConfig::POSITIONING_MOVEMENT_GYRO;

    // Wait for positioning movement at least once.
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
      // Discard previous baseline samples and restart
      // the three-second baseline timer.
      repCounter.begin(currentTime);
      return;
    }

    // Only stationary samples enter the baseline average.
    repCounter.update(filteredMagnitude, currentTime);

    if (repCounter.state() == MOVEMENT_CALIBRATION) {
      startCalibrationReadyBlink(currentTime);
    }

    return;
  }

  // Baseline and ready indication have completed.
  repCounter.update(filteredMagnitude, currentTime);

  if (repCounter.consumeRepEvent()) {
    digitalWrite(
      GeneralConfig::LED_PIN,
      GeneralConfig::LED_ON
    );

    ledActive = true;
    ledStartTime = currentTime;
  }
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);

  pinMode(GeneralConfig::LED_PIN, OUTPUT);
  digitalWrite(
    GeneralConfig::LED_PIN,
    GeneralConfig::LED_OFF
  );

  pinMode(FallConfig::BUZZER_PIN, OUTPUT);
  digitalWrite(FallConfig::BUZZER_PIN, LOW);

  delay(1000);

  if (!mpu.begin()) {
    Serial.println("MPU6050_Error:0");

    while (true) {
      digitalWrite(
        GeneralConfig::LED_PIN,
        GeneralConfig::LED_ON
      );
      delay(200);

      digitalWrite(
        GeneralConfig::LED_PIN,
        GeneralConfig::LED_OFF
      );
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
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  unsigned long currentTime = millis();

  // Service LED and buzzer even between sensor samples.
  updateOutputTimers(currentTime);

  const unsigned long sampleTime = micros();

  if (
    sampleTime - lastSampleTime <
    GeneralConfig::SAMPLE_INTERVAL_US
  ) {
    return;
  }

  // Do not generate catch-up bursts if a loop runs late.
  lastSampleTime = sampleTime;

  sensors_event_t acceleration;
  sensors_event_t gyroscope;
  sensors_event_t temperature;

  mpu.getEvent(
    &acceleration,
    &gyroscope,
    &temperature
  );

  currentTime = millis();

  const float rawMagnitude = sqrt(
    acceleration.acceleration.x *
      acceleration.acceleration.x +
    acceleration.acceleration.y *
      acceleration.acceleration.y +
    acceleration.acceleration.z *
      acceleration.acceleration.z
  );

  const float gyroMagnitude = sqrt(
    gyroscope.gyro.x * gyroscope.gyro.x +
    gyroscope.gyro.y * gyroscope.gyro.y +
    gyroscope.gyro.z * gyroscope.gyro.z
  );

  // ----------------------------------------------------
  // Fall detection remains active during preparation,
  // baseline collection, blinking and rep counting.
  // Uses magnitude before the additional EMA filter.
  // ----------------------------------------------------

  fallDetector.update(rawMagnitude, currentTime);

  if (fallDetector.consumeFallEvent()) {
    digitalWrite(FallConfig::BUZZER_PIN, HIGH);
    buzzerActive = true;
    buzzerStartTime = currentTime;
  }

  // ----------------------------------------------------
  // Acceleration filtering
  // ----------------------------------------------------

  if (!filterInitialised) {
    filteredMagnitude = rawMagnitude;
    filterInitialised = true;
  } else {
    filteredMagnitude =
      GeneralConfig::FILTER_ALPHA * rawMagnitude +
      (1.0 - GeneralConfig::FILTER_ALPHA) *
        filteredMagnitude;
  }

  // ----------------------------------------------------
  // Preparation, baseline and exercise processing
  // ----------------------------------------------------

  updateRepCounting(gyroMagnitude, currentTime);

  // ----------------------------------------------------
  // Wireless telemetry
  // ----------------------------------------------------

  if (wirelessStarted) {
    wirelessStreamer.update(
      rawMagnitude,
      filteredMagnitude,
      repCounter,
      fallDetector,
      buzzerActive,
      SELECTED_EXERCISE,
      currentTime
    );
  }

  // Wired USB telemetry.
  printOutput(rawMagnitude);
}