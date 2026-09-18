#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

#include "ExerciseConfig.h"
#include "RepCounter.h"
#include "ContinuousCounter.h"
#include "FallDetector.h"
#include "ExerciseRecognition.h"
#include "WirelessStreamer.h"

Adafruit_MPU6050 mpu;

ExerciseSettings genericSettings = getGenericSettings();

RepCounter repCounter(genericSettings);
ContinuousCounter continuousCounter;

FallDetector fallDetector;
ExerciseRecognition exerciseRecognition;
WirelessStreamer wirelessStreamer;

bool wirelessStarted = false;

// AUTO_MODE means no detector has been selected yet.
CountingMode activeMode = AUTO_MODE;

// Ignore any internal counts accumulated before mode selection.
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

// True quiet interval for AUTO mode selection.
bool quietSelectionActive = false;
unsigned long quietSelectionStart = 0;

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

  return activeMode == AUTO_MODE ? 1 : 2;
}

int calibrationProgress() {
  if (sessionState() == 0) {
    return 0;
  }

  if (activeMode != AUTO_MODE) {
    return 2;
  }

  int progress = 0;

  if (ModeConfig::REQUESTED_MODE != CONTINUOUS_MODE) {
    progress = repCounter.calibrationCount();
  }

  if (
    ModeConfig::REQUESTED_MODE != PAUSE_MODE &&
    continuousCounter.calibrationCount() > progress
  ) {
    progress = continuousCounter.calibrationCount();
  }

  // Report completion only after a mode has been selected.
  return progress > 0 ? 1 : 0;
}

int reportedRepetitions() {
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
      ModeConfig::REQUESTED_MODE == AUTO_MODE
    );

    exerciseRecognition.beginCalibration(now);

    quietSelectionActive = false;

    calibrationLedOffPulse = false;
    ledActive = false;
    setLed(true);
  }
}

void selectMode(CountingMode mode) {
  activeMode = mode;

  if (mode == PAUSE_MODE) {
    repetitionOffset = repCounter.repetitionCount();
  } else {
    repetitionOffset = continuousCounter.repetitionCount();
  }

  calibrationLedOffPulse = false;
  ledActive = false;
  setLed(false);
}

void processMovement(
  const float acceleration[3],
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

  if (stationary) {
    if (!quietSelectionActive) {
      quietSelectionActive = true;
      quietSelectionStart = now;
    }
  } else {
    quietSelectionActive = false;
  }

  const bool stillCalibrating = activeMode == AUTO_MODE;

  const bool runPauseDetector =
    activeMode == PAUSE_MODE ||
    (
      stillCalibrating &&
      ModeConfig::REQUESTED_MODE != CONTINUOUS_MODE
    );

  const bool runContinuousDetector =
    activeMode == CONTINUOUS_MODE ||
    (
      stillCalibrating &&
      ModeConfig::REQUESTED_MODE != PAUSE_MODE
    );

  if (runPauseDetector) {
    repCounter.update(filteredMagnitude, now);
  }

  if (runContinuousDetector) {
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

  if (stillCalibrating) {
    const bool pauseReady =
      repCounter.state() == REP_COUNTING;

    const bool continuousReady =
      continuousCounter.calibrated();

    if (ModeConfig::REQUESTED_MODE == PAUSE_MODE) {
      if (pauseReady) {
        selectMode(PAUSE_MODE);
      }
    } else if (
      ModeConfig::REQUESTED_MODE == CONTINUOUS_MODE
    ) {
      if (continuousReady) {
        selectMode(CONTINUOUS_MODE);
      }
    } else {
      const bool genuineQuietInterval =
        quietSelectionActive &&
        now - quietSelectionStart >=
          GeneralConfig::QUIET_TIME_MS;

      // Prefer the existing detector when its candidates
      // match and the signal genuinely settles.
      if (pauseReady && genuineQuietInterval) {
        selectMode(PAUSE_MODE);
      } else if (continuousReady) {
        selectMode(CONTINUOUS_MODE);
      }
    }

    // Recognition remains independent of counting mode.
    exerciseRecognition.update(
      acceleration,
      gyro,
      calibrationProgress(),
      now
    );

    if (activeMode != AUTO_MODE) {
      exerciseRecognition.endCalibration();
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

TelemetryData makeTelemetry(float rawMagnitude) {
  TelemetryData data = {};

  data.raw = rawMagnitude;
  data.filtered = filteredMagnitude;

  data.motion = fabsf(
    filteredMagnitude - repCounter.baselineMagnitude()
  );

  data.high = repCounter.highThreshold();
  data.low = repCounter.lowThreshold();
  data.requiredPeak = repCounter.requiredRepPeak();
  data.lastPeak = repCounter.lastCompletedPeak();

  data.lastTimeS =
    repCounter.lastCompletedDuration() / 1000.0f;

  data.gyroSignal = continuousCounter.signal();
  data.gyroHigh = continuousCounter.startGate();
  data.requiredAngle = continuousCounter.requiredAngle();
  data.lastAngle = continuousCounter.lastAngle();

  data.cycleTimeS =
    continuousCounter.lastDuration() / 1000.0f;

  data.reps = reportedRepetitions();
  data.calibrationReps = calibrationProgress();

  data.pauseCandidates = repCounter.calibrationCount();
  data.continuousCandidates =
    continuousCounter.calibrationCount();

  data.repState = sessionState();
  data.mode = (int)activeMode;
  data.requestedMode = (int)ModeConfig::REQUESTED_MODE;
  data.gyroAxis = continuousCounter.axis();

  data.fallState = (int)fallDetector.state();
  data.buzzer = buzzerActive;

  data.exercise = (int)exerciseRecognition.exercise();
  data.recognitionActive = exerciseRecognition.active();
  data.recognitionMock = exerciseRecognition.usingMock();

  return data;
}

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

  const unsigned long now = millis();

  restartBaseline(now);
  fallDetector.begin(now);

  lastSampleTime = micros();
  lastUsbOutputTime = now;
}

void loop() {
  unsigned long now = millis();

  updateOutputTimers(now);

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

  now = millis();

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
    processMovement(acceleration, gyro, now);
  }

  const TelemetryData data = makeTelemetry(rawMagnitude);

  if (wirelessStarted) {
    wirelessStreamer.update(data, now);
  }

  if (
    now - lastUsbOutputTime >=
    GeneralConfig::OUTPUT_INTERVAL_MS
  ) {
    lastUsbOutputTime = now;
    printTelemetry(Serial, data);
  }
}