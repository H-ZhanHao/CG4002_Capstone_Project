#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "FallDetector.h"

Adafruit_MPU6050 mpu;

// ======================================================
// SELECTED EXERCISE
// ======================================================

const ExerciseType SELECTED_EXERCISE =
  SQUAT;

ExerciseSettings exerciseSettings =
  getExerciseSettings(
    SELECTED_EXERCISE
  );

// ======================================================
// ALGORITHM OBJECTS
// ======================================================

RepCounter repCounter(
  exerciseSettings
);

FallDetector fallDetector;

// ======================================================
// SAMPLING AND FILTERING
// ======================================================

unsigned long lastSampleTime = 0;

float filteredMagnitude = 0.0;
bool filterInitialised = false;

// ======================================================
// LED STATE
// ======================================================

bool ledActive = false;
unsigned long ledStartTime = 0;

// ======================================================
// BUZZER STATE
// ======================================================

bool buzzerActive = false;
unsigned long buzzerStartTime = 0;

// ======================================================
// SERIAL OUTPUT
// ======================================================

void printOutput(
  float rawMagnitude
) {
  Serial.print("Raw:");
  Serial.print(rawMagnitude, 3);

  Serial.print("\tFiltered:");
  Serial.print(filteredMagnitude, 3);

  Serial.print("\tMotion:");
  Serial.print(
    repCounter.motionSignal(),
    3
  );

  Serial.print("\tHigh:");
  Serial.print(
    repCounter.highThreshold(),
    3
  );

  Serial.print("\tLow:");
  Serial.print(
    repCounter.lowThreshold(),
    3
  );

  Serial.print("\tRequiredPeak:");
  Serial.print(
    repCounter.requiredRepPeak(),
    3
  );

  Serial.print("\tLastPeak:");
  Serial.print(
    repCounter.lastCompletedPeak(),
    3
  );

  Serial.print("\tMinTimeS:");
  Serial.print(
    repCounter.minimumRepDuration() /
    1000.0,
    2
  );

  Serial.print("\tMaxTimeS:");
  Serial.print(
    repCounter.maximumRepDuration() /
    1000.0,
    2
  );

  Serial.print("\tLastTimeS:");
  Serial.print(
    repCounter.lastCompletedDuration() /
    1000.0,
    2
  );

  Serial.print("\tReps:");
  Serial.print(
    repCounter.repetitionCount()
  );

  Serial.print("\tCalibrationReps:");
  Serial.print(
    repCounter.calibrationCount()
  );

  Serial.print("\tRepState:");
  Serial.print(
    (int)repCounter.state()
  );

  Serial.print("\tFallState:");
  Serial.print(
    (int)fallDetector.state()
  );

  Serial.print("\tBuzzer:");
  Serial.print(
    buzzerActive ? 1 : 0
  );

  Serial.print("\tExercise:");
  Serial.println(
    (int)SELECTED_EXERCISE
  );
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  // ----------------------------------------------------
  // LED setup
  // ----------------------------------------------------

  pinMode(
    GeneralConfig::LED_PIN,
    OUTPUT
  );

  digitalWrite(
    GeneralConfig::LED_PIN,
    LOW
  );

  // ----------------------------------------------------
  // Buzzer setup
  // ----------------------------------------------------

  pinMode(
    FallConfig::BUZZER_PIN,
    OUTPUT
  );

  digitalWrite(
    FallConfig::BUZZER_PIN,
    LOW
  );

  // ----------------------------------------------------
  // MPU6050 setup
  // ----------------------------------------------------

  if (!mpu.begin()) {
    Serial.println(
      "MPU6050_Error:0"
    );

    while (1) {
      digitalWrite(
        GeneralConfig::LED_PIN,
        HIGH
      );

      delay(200);

      digitalWrite(
        GeneralConfig::LED_PIN,
        LOW
      );

      delay(200);
    }
  }

  mpu.setAccelerometerRange(
    MPU6050_RANGE_8_G
  );

  mpu.setGyroRange(
    MPU6050_RANGE_500_DEG
  );

  mpu.setFilterBandwidth(
    MPU6050_BAND_21_HZ
  );

  // ----------------------------------------------------
  // Algorithm initialisation
  // ----------------------------------------------------

  unsigned long currentTime =
    millis();

  repCounter.begin(
    currentTime
  );

  fallDetector.begin(
    currentTime
  );

  lastSampleTime =
    micros();
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  unsigned long currentTime =
    millis();

  // ----------------------------------------------------
  // Turn off LED after 200 ms
  // ----------------------------------------------------

  if (
    ledActive &&
    currentTime - ledStartTime >= 200
  ) {
    digitalWrite(
      GeneralConfig::LED_PIN,
      LOW
    );

    ledActive = false;
  }

  // ----------------------------------------------------
  // Turn off buzzer after alarm duration
  // ----------------------------------------------------

  if (
    buzzerActive &&
    currentTime - buzzerStartTime >=
      FallConfig::BUZZER_DURATION_MS
  ) {
    digitalWrite(
      FallConfig::BUZZER_PIN,
      LOW
    );

    buzzerActive = false;
  }

  // ----------------------------------------------------
  // Maintain 50 Hz sampling
  // ----------------------------------------------------

  if (
    micros() - lastSampleTime <
    GeneralConfig::SAMPLE_INTERVAL_US
  ) {
    return;
  }

  lastSampleTime +=
    GeneralConfig::SAMPLE_INTERVAL_US;

  // ----------------------------------------------------
  // Read MPU6050
  // ----------------------------------------------------

  sensors_event_t acceleration;
  sensors_event_t gyroscope;
  sensors_event_t temperature;

  mpu.getEvent(
    &acceleration,
    &gyroscope,
    &temperature
  );

  // Calculate total acceleration magnitude
  float rawMagnitude = sqrt(
    acceleration.acceleration.x *
    acceleration.acceleration.x +

    acceleration.acceleration.y *
    acceleration.acceleration.y +

    acceleration.acceleration.z *
    acceleration.acceleration.z
  );

  // ----------------------------------------------------
  // Fall detection
  //
  // Use raw magnitude because filtering could reduce
  // the short impact peak.
  // ----------------------------------------------------

  fallDetector.update(
    rawMagnitude,
    currentTime
  );

  if (
    fallDetector.consumeFallEvent()
  ) {
    digitalWrite(
      FallConfig::BUZZER_PIN,
      HIGH
    );

    buzzerActive = true;
    buzzerStartTime = currentTime;
  }

  // ----------------------------------------------------
  // Exponential moving-average filter
  // ----------------------------------------------------

  if (!filterInitialised) {
    filteredMagnitude =
      rawMagnitude;

    filterInitialised = true;
  } else {
    filteredMagnitude =
      GeneralConfig::FILTER_ALPHA *
      rawMagnitude +

      (
        1.0 -
        GeneralConfig::FILTER_ALPHA
      ) *
      filteredMagnitude;
  }

  // ----------------------------------------------------
  // Rep-counting algorithm
  // ----------------------------------------------------

  repCounter.update(
    filteredMagnitude,
    currentTime
  );

  // Flash LED whenever a valid rep is counted
  if (
    repCounter.consumeRepEvent()
  ) {
    digitalWrite(
      GeneralConfig::LED_PIN,
      HIGH
    );

    ledActive = true;
    ledStartTime = currentTime;
  }

  // ----------------------------------------------------
  // Serial Monitor and Serial Plotter output
  // ----------------------------------------------------

  printOutput(
    rawMagnitude
  );
}