#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"

Adafruit_MPU6050 mpu;

const ExerciseType SELECTED_EXERCISE =
  PUSH_UP;

ExerciseSettings exerciseSettings =
  getExerciseSettings(
    SELECTED_EXERCISE
  );

RepCounter repCounter(
  exerciseSettings
);

unsigned long lastSampleTime = 0;

float filteredMagnitude = 0.0;
bool filterInitialised = false;

bool ledActive = false;
unsigned long ledStartTime = 0;

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

  Serial.print("\tState:");
  Serial.print(
    (int)repCounter.state()
  );

  Serial.print("\tExercise:");
  Serial.println(
    (int)SELECTED_EXERCISE
  );
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(
    GeneralConfig::LED_PIN,
    OUTPUT
  );

  digitalWrite(
    GeneralConfig::LED_PIN,
    LOW
  );

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

  unsigned long currentTime =
    millis();

  repCounter.begin(currentTime);

  lastSampleTime = micros();
}

void loop() {
  unsigned long currentTime =
    millis();

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

  if (
    micros() - lastSampleTime <
    GeneralConfig::SAMPLE_INTERVAL_US
  ) {
    return;
  }

  lastSampleTime +=
    GeneralConfig::SAMPLE_INTERVAL_US;

  sensors_event_t acceleration;
  sensors_event_t gyroscope;
  sensors_event_t temperature;

  mpu.getEvent(
    &acceleration,
    &gyroscope,
    &temperature
  );

  float rawMagnitude = sqrt(
    acceleration.acceleration.x *
    acceleration.acceleration.x +

    acceleration.acceleration.y *
    acceleration.acceleration.y +

    acceleration.acceleration.z *
    acceleration.acceleration.z
  );

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

  repCounter.update(
    filteredMagnitude,
    currentTime
  );

  if (repCounter.consumeRepEvent()) {
    digitalWrite(
      GeneralConfig::LED_PIN,
      HIGH
    );

    ledActive = true;
    ledStartTime = currentTime;
  }

  printOutput(rawMagnitude);
}