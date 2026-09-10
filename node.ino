// Dynamic squat calibration and local rep counting
// Hardware: ESP32 + MPU-6050
// Sensor position: Hip

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

Adafruit_MPU6050 mpu;

// ======================================================
// SETTINGS
// ======================================================

// Harmless if no LED is connected
const int LED_PIN = 2;

// 50 Hz sampling
const unsigned long SAMPLE_INTERVAL_US = 20000;

// Lower value gives a smoother signal
const float FILTER_ALPHA = 0.20;

// Remain still for two seconds after reset
const unsigned long BASELINE_TIME_MS = 2000;

// Temporary gates used only during calibration
const float CALIBRATION_START_GATE = 0.80;
const float CALIBRATION_END_GATE = 0.30;

// Three similar calibration squats are required
const int REQUIRED_STABLE_REPS = 3;

// Calibration squats must have similar peaks and durations
const float PEAK_SIMILARITY_TOLERANCE = 0.25;
const float DURATION_SIMILARITY_TOLERANCE = 0.35;

// Safeguards for accepting a calibration movement
const float MINIMUM_VALID_CALIBRATION_PEAK = 1.00;
const unsigned long CALIBRATION_MIN_DURATION_MS = 700;
const unsigned long CALIBRATION_MAX_DURATION_MS = 6000;

// Final thresholds calculated from calibration
const float HIGH_THRESHOLD_FACTOR = 0.55;
const float LOW_THRESHOLD_FACTOR = 0.20;
const float REQUIRED_PEAK_FACTOR = 0.70;

// Final timing limits calculated from calibration
const float MIN_DURATION_FACTOR = 0.60;
const float MAX_DURATION_FACTOR = 1.60;

// End movement after returning near baseline
const unsigned long QUIET_TIME_MS = 350;

// Minimum delay before another movement starts
const unsigned long MIN_REP_GAP_MS = 300;

// ======================================================
// SYSTEM STATES
// ======================================================

enum SystemState {
  BASELINE_CALIBRATION = 0,
  MOVEMENT_CALIBRATION = 1,
  REP_COUNTING = 2
};

SystemState systemState = BASELINE_CALIBRATION;

// ======================================================
// SENSOR AND FILTER VARIABLES
// ======================================================

unsigned long lastSampleTime = 0;

float filteredMagnitude = 0.0;
bool filterInitialised = false;

// ======================================================
// BASELINE CALIBRATION VARIABLES
// ======================================================

unsigned long baselineStartTime = 0;

float baselineSum = 0.0;
unsigned int baselineSamples = 0;

float baselineMagnitude = 9.81;

// ======================================================
// MOVEMENT CALIBRATION VARIABLES
// ======================================================

float calibrationPeaks[REQUIRED_STABLE_REPS];

unsigned long calibrationDurations[
  REQUIRED_STABLE_REPS
];

int storedCalibrationReps = 0;

// Final calculated thresholds
float highThreshold = 0.0;
float lowThreshold = 0.0;
float requiredRepPeak = 0.0;

// Final calculated duration limits
unsigned long minimumRepDuration = 0;
unsigned long maximumRepDuration = 0;

unsigned long lastCompletedDuration = 0;

// ======================================================
// MOVEMENT TRACKING VARIABLES
// ======================================================

bool movementActive = false;

float movementPeak = 0.0;

unsigned long movementStartTime = 0;
unsigned long quietStartTime = 0;
unsigned long lastMovementEndTime = 0;

// ======================================================
// REP COUNT AND LED VARIABLES
// ======================================================

int repetitionCount = 0;

bool ledActive = false;
unsigned long ledStartTime = 0;

// ======================================================
// RESET CURRENT MOVEMENT
// ======================================================

void resetMovementCycle() {
  movementActive = false;
  movementPeak = 0.0;
  movementStartTime = 0;
  quietStartTime = 0;
}

// ======================================================
// STORE CALIBRATION MOVEMENT
// ======================================================

void saveCalibrationMovement(
  float peak,
  unsigned long duration
) {
  if (storedCalibrationReps <
      REQUIRED_STABLE_REPS) {
    calibrationPeaks[storedCalibrationReps] =
      peak;

    calibrationDurations[
      storedCalibrationReps
    ] = duration;

    storedCalibrationReps++;
  } else {
    // Remove oldest movement
    calibrationPeaks[0] =
      calibrationPeaks[1];

    calibrationPeaks[1] =
      calibrationPeaks[2];

    calibrationPeaks[2] = peak;

    calibrationDurations[0] =
      calibrationDurations[1];

    calibrationDurations[1] =
      calibrationDurations[2];

    calibrationDurations[2] = duration;
  }
}

// ======================================================
// CHECK CALIBRATION STABILITY
// ======================================================

bool calibrationMovementsAreStable() {
  if (storedCalibrationReps <
      REQUIRED_STABLE_REPS) {
    return false;
  }

  float averagePeak =
    (
      calibrationPeaks[0] +
      calibrationPeaks[1] +
      calibrationPeaks[2]
    ) / 3.0;

  float averageDuration =
    (
      calibrationDurations[0] +
      calibrationDurations[1] +
      calibrationDurations[2]
    ) / 3.0;

  if (averagePeak <= 0.0 ||
      averageDuration <= 0.0) {
    return false;
  }

  for (int i = 0;
       i < REQUIRED_STABLE_REPS;
       i++) {
    float peakDifference =
      fabs(
        calibrationPeaks[i] -
        averagePeak
      );

    float durationDifference =
      fabs(
        (float)calibrationDurations[i] -
        averageDuration
      );

    if (
      peakDifference >
      PEAK_SIMILARITY_TOLERANCE *
      averagePeak
    ) {
      return false;
    }

    if (
      durationDifference >
      DURATION_SIMILARITY_TOLERANCE *
      averageDuration
    ) {
      return false;
    }
  }

  return true;
}

// ======================================================
// CALCULATE FINAL LIMITS
// ======================================================

void finishMovementCalibration() {
  float averagePeak =
    (
      calibrationPeaks[0] +
      calibrationPeaks[1] +
      calibrationPeaks[2]
    ) / 3.0;

  float averageDuration =
    (
      calibrationDurations[0] +
      calibrationDurations[1] +
      calibrationDurations[2]
    ) / 3.0;

  // Movement start and end thresholds
  highThreshold =
    averagePeak * HIGH_THRESHOLD_FACTOR;

  lowThreshold =
    averagePeak * LOW_THRESHOLD_FACTOR;

  // Required amplitude for a valid repetition
  requiredRepPeak =
    averagePeak * REQUIRED_PEAK_FACTOR;

  // Prevent thresholds becoming too sensitive
  if (highThreshold < 0.45) {
    highThreshold = 0.45;
  }

  if (lowThreshold < 0.20) {
    lowThreshold = 0.20;
  }

  if (
    requiredRepPeak <
    MINIMUM_VALID_CALIBRATION_PEAK
  ) {
    requiredRepPeak =
      MINIMUM_VALID_CALIBRATION_PEAK;
  }

  // Dynamically calculate timing limits
  minimumRepDuration =
    (unsigned long)(
      averageDuration *
      MIN_DURATION_FACTOR
    );

  maximumRepDuration =
    (unsigned long)(
      averageDuration *
      MAX_DURATION_FACTOR
    );

  // Broad safety bounds
  if (minimumRepDuration < 500) {
    minimumRepDuration = 500;
  }

  if (maximumRepDuration > 8000) {
    maximumRepDuration = 8000;
  }

  if (
    maximumRepDuration <=
    minimumRepDuration
  ) {
    maximumRepDuration =
      minimumRepDuration + 500;
  }

  repetitionCount = 0;
  systemState = REP_COUNTING;

  resetMovementCycle();
}

// ======================================================
// HANDLE COMPLETED MOVEMENT
// ======================================================

void completeMovement(
  unsigned long currentTime
) {
  unsigned long movementDuration =
    currentTime - movementStartTime;

  float completedPeak = movementPeak;

  lastCompletedDuration =
    movementDuration;

  resetMovementCycle();

  lastMovementEndTime = currentTime;

  if (
    systemState ==
    MOVEMENT_CALIBRATION
  ) {
    // Reject invalid calibration movement
    if (
      movementDuration <
      CALIBRATION_MIN_DURATION_MS
    ) {
      return;
    }

    if (
      movementDuration >
      CALIBRATION_MAX_DURATION_MS
    ) {
      return;
    }

    if (
      completedPeak <
      MINIMUM_VALID_CALIBRATION_PEAK
    ) {
      return;
    }

    saveCalibrationMovement(
      completedPeak,
      movementDuration
    );

    if (
      calibrationMovementsAreStable()
    ) {
      finishMovementCalibration();
    }
  }

  else if (
    systemState ==
    REP_COUNTING
  ) {
    // Must reach calibrated movement amplitude
    if (
      completedPeak <
      requiredRepPeak
    ) {
      return;
    }

    // Must fit calibrated timing range
    if (
      movementDuration <
      minimumRepDuration
    ) {
      return;
    }

    if (
      movementDuration >
      maximumRepDuration
    ) {
      return;
    }

    repetitionCount++;

    // Harmless if no LED is connected
    digitalWrite(LED_PIN, HIGH);

    ledActive = true;
    ledStartTime = currentTime;
  }
}

// ======================================================
// MOVEMENT DETECTION
// ======================================================

void processMovement(
  float motionSignal,
  float startThreshold,
  float endThreshold,
  unsigned long currentTime
) {
  if (!movementActive) {
    bool enoughTimePassed =
      currentTime -
      lastMovementEndTime >=
      MIN_REP_GAP_MS;

    if (
      motionSignal > startThreshold &&
      enoughTimePassed
    ) {
      movementActive = true;

      movementStartTime =
        currentTime;

      movementPeak =
        motionSignal;

      quietStartTime = 0;
    }

    return;
  }

  // Store largest movement value
  if (motionSignal > movementPeak) {
    movementPeak = motionSignal;
  }

  unsigned long activeMaximumDuration;

  if (systemState == REP_COUNTING) {
    activeMaximumDuration =
      maximumRepDuration;
  } else {
    activeMaximumDuration =
      CALIBRATION_MAX_DURATION_MS;
  }

  // Cancel an abnormally long movement
  if (
    currentTime - movementStartTime >
    activeMaximumDuration
  ) {
    resetMovementCycle();

    lastMovementEndTime =
      currentTime;

    return;
  }

  // Check whether movement returned to rest
  if (motionSignal < endThreshold) {
    if (quietStartTime == 0) {
      quietStartTime = currentTime;
    }

    if (
      currentTime - quietStartTime >=
      QUIET_TIME_MS
    ) {
      completeMovement(currentTime);
    }
  } else {
    quietStartTime = 0;
  }
}

// ======================================================
// SERIAL MONITOR / PLOTTER OUTPUT
// ======================================================

void printOutput(
  float rawMagnitude,
  float filteredValue,
  float motionSignal
) {
  Serial.print("Raw:");
  Serial.print(rawMagnitude, 3);

  Serial.print("\tFiltered:");
  Serial.print(filteredValue, 3);

  Serial.print("\tMotion:");
  Serial.print(motionSignal, 3);

  Serial.print("\tHigh:");
  Serial.print(highThreshold, 3);

  Serial.print("\tLow:");
  Serial.print(lowThreshold, 3);

  Serial.print("\tRequiredPeak:");
  Serial.print(requiredRepPeak, 3);

  Serial.print("\tMinTimeS:");
  Serial.print(
    minimumRepDuration / 1000.0,
    2
  );

  Serial.print("\tMaxTimeS:");
  Serial.print(
    maximumRepDuration / 1000.0,
    2
  );

  Serial.print("\tLastTimeS:");
  Serial.print(
    lastCompletedDuration / 1000.0,
    2
  );

  Serial.print("\tReps:");
  Serial.print(repetitionCount);

  Serial.print("\tCalibrationReps:");
  Serial.print(storedCalibrationReps);

  Serial.print("\tState:");
  Serial.println((int)systemState);
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!mpu.begin()) {
    Serial.println("MPU6050_Error:0");

    while (1) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);

      digitalWrite(LED_PIN, LOW);
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

  baselineStartTime = millis();
  lastSampleTime = micros();
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  unsigned long currentTime = millis();

  // Turn LED off without stopping sampling
  if (
    ledActive &&
    currentTime - ledStartTime >= 200
  ) {
    digitalWrite(LED_PIN, LOW);
    ledActive = false;
  }

  // Maintain approximately 50 Hz
  if (
    micros() - lastSampleTime <
    SAMPLE_INTERVAL_US
  ) {
    return;
  }

  lastSampleTime +=
    SAMPLE_INTERVAL_US;

  sensors_event_t acceleration;
  sensors_event_t gyroscope;
  sensors_event_t temperature;

  mpu.getEvent(
    &acceleration,
    &gyroscope,
    &temperature
  );

  // Calculate acceleration magnitude
  float rawMagnitude = sqrt(
    acceleration.acceleration.x *
    acceleration.acceleration.x +

    acceleration.acceleration.y *
    acceleration.acceleration.y +

    acceleration.acceleration.z *
    acceleration.acceleration.z
  );

  // Exponential moving-average filter
  if (!filterInitialised) {
    filteredMagnitude =
      rawMagnitude;

    filterInitialised = true;
  } else {
    filteredMagnitude =
      FILTER_ALPHA *
      rawMagnitude +

      (1.0 - FILTER_ALPHA) *
      filteredMagnitude;
  }

  // Movement relative to stationary baseline
  float motionSignal =
    fabs(
      filteredMagnitude -
      baselineMagnitude
    );

  // State 0: stationary calibration
  if (
    systemState ==
    BASELINE_CALIBRATION
  ) {
    baselineSum +=
      filteredMagnitude;

    baselineSamples++;

    if (
      currentTime -
      baselineStartTime >=
      BASELINE_TIME_MS
    ) {
      baselineMagnitude =
        baselineSum /
        baselineSamples;

      systemState =
        MOVEMENT_CALIBRATION;

      resetMovementCycle();
    }
  }

  // State 1: squat calibration
  else if (
    systemState ==
    MOVEMENT_CALIBRATION
  ) {
    processMovement(
      motionSignal,
      CALIBRATION_START_GATE,
      CALIBRATION_END_GATE,
      currentTime
    );
  }

  // State 2: normal rep counting
  else if (
    systemState ==
    REP_COUNTING
  ) {
    processMovement(
      motionSignal,
      highThreshold,
      lowThreshold,
      currentTime
    );
  }

  printOutput(
    rawMagnitude,
    filteredMagnitude,
    motionSignal
  );
}