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

const int LED_PIN = 2;

// 50 Hz sampling
const unsigned long SAMPLE_INTERVAL_US = 20000;

// Filtering: lower value = smoother signal
const float FILTER_ALPHA = 0.20;

// User must remain still during this period
const unsigned long BASELINE_TIME_MS = 2000;

// Temporary calibration gates in m/s²
const float CALIBRATION_START_GATE = 0.80;
const float CALIBRATION_END_GATE = 0.30;

// Three similar calibration squats are required
const int REQUIRED_STABLE_REPS = 3;

// Peaks must be within ±25% of their average
const float SIMILARITY_TOLERANCE = 0.25;

// Reject movements that are too small
const float MINIMUM_VALID_PEAK = 1.00;

// Movement must remain near baseline before the squat ends
const unsigned long QUIET_TIME_MS = 350;

// Reject movements that are too short or too long
const unsigned long MIN_REP_DURATION_MS = 700;
const unsigned long MAX_REP_DURATION_MS = 6000;

// Minimum gap before another movement can begin
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
int storedCalibrationReps = 0;

float highThreshold = 0.0;
float lowThreshold = 0.0;

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
// STORE LATEST CALIBRATION PEAK
// ======================================================

void saveCalibrationPeak(float peak) {
  if (storedCalibrationReps < REQUIRED_STABLE_REPS) {
    calibrationPeaks[storedCalibrationReps] = peak;
    storedCalibrationReps++;
  } else {
    // Remove oldest value and keep latest three
    calibrationPeaks[0] = calibrationPeaks[1];
    calibrationPeaks[1] = calibrationPeaks[2];
    calibrationPeaks[2] = peak;
  }
}

// ======================================================
// CHECK WHETHER THREE PEAKS ARE SIMILAR
// ======================================================

bool calibrationPeaksAreStable() {
  if (storedCalibrationReps < REQUIRED_STABLE_REPS) {
    return false;
  }

  float averagePeak =
    (calibrationPeaks[0] +
     calibrationPeaks[1] +
     calibrationPeaks[2]) / 3.0;

  if (averagePeak <= 0.0) {
    return false;
  }

  for (int i = 0; i < REQUIRED_STABLE_REPS; i++) {
    float difference =
      fabs(calibrationPeaks[i] - averagePeak);

    float allowedDifference =
      SIMILARITY_TOLERANCE * averagePeak;

    if (difference > allowedDifference) {
      return false;
    }
  }

  return true;
}

// ======================================================
// CALCULATE FINAL THRESHOLDS
// ======================================================

void finishMovementCalibration() {
  float averagePeak =
    (calibrationPeaks[0] +
     calibrationPeaks[1] +
     calibrationPeaks[2]) / 3.0;

  // Hysteresis thresholds
  highThreshold = averagePeak * 0.45;
  lowThreshold = averagePeak * 0.20;

  // Prevent thresholds from becoming too sensitive
  if (highThreshold < 0.45) {
    highThreshold = 0.45;
  }

  if (lowThreshold < 0.20) {
    lowThreshold = 0.20;
  }

  repetitionCount = 0;
  systemState = REP_COUNTING;

  resetMovementCycle();
}

// ======================================================
// HANDLE A COMPLETED MOVEMENT
// ======================================================

void completeMovement(unsigned long currentTime) {
  unsigned long movementDuration =
    currentTime - movementStartTime;

  float completedPeak = movementPeak;

  resetMovementCycle();
  lastMovementEndTime = currentTime;

  // Reject invalid movements
  if (movementDuration < MIN_REP_DURATION_MS) {
    return;
  }

  if (movementDuration > MAX_REP_DURATION_MS) {
    return;
  }

  if (completedPeak < MINIMUM_VALID_PEAK) {
    return;
  }

  if (systemState == MOVEMENT_CALIBRATION) {
    saveCalibrationPeak(completedPeak);

    if (calibrationPeaksAreStable()) {
      finishMovementCalibration();
    }
  }

  else if (systemState == REP_COUNTING) {
    repetitionCount++;

    // LED feedback for a counted repetition
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
      currentTime - lastMovementEndTime >= MIN_REP_GAP_MS;

    if (motionSignal > startThreshold &&
        enoughTimePassed) {
      movementActive = true;
      movementStartTime = currentTime;
      movementPeak = motionSignal;
      quietStartTime = 0;
    }

    return;
  }

  // Update peak value during movement
  if (motionSignal > movementPeak) {
    movementPeak = motionSignal;
  }

  // Cancel an abnormally long movement
  if (currentTime - movementStartTime >
      MAX_REP_DURATION_MS) {
    resetMovementCycle();
    lastMovementEndTime = currentTime;
    return;
  }

  // Check whether user has returned to rest
  if (motionSignal < endThreshold) {
    if (quietStartTime == 0) {
      quietStartTime = currentTime;
    }

    if (currentTime - quietStartTime >=
        QUIET_TIME_MS) {
      completeMovement(currentTime);
    }
  } else {
    quietStartTime = 0;
  }
}

// ======================================================
// SERIAL PLOTTER OUTPUT
// ======================================================

void printPlotterData(
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
    // Flash continuously if MPU-6050 cannot be found
    while (1) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);

      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  baselineStartTime = millis();
  lastSampleTime = micros();
}

// ======================================================
// MAIN LOOP
// ======================================================

void loop() {
  unsigned long currentTime = millis();

  // Turn LED off after 200 ms without stopping sampling
  if (ledActive &&
      currentTime - ledStartTime >= 200) {
    digitalWrite(LED_PIN, LOW);
    ledActive = false;
  }

  // Maintain approximately 50 Hz sampling
  if (micros() - lastSampleTime <
      SAMPLE_INTERVAL_US) {
    return;
  }

  lastSampleTime += SAMPLE_INTERVAL_US;

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
    filteredMagnitude = rawMagnitude;
    filterInitialised = true;
  } else {
    filteredMagnitude =
      FILTER_ALPHA * rawMagnitude +
      (1.0 - FILTER_ALPHA) *
      filteredMagnitude;
  }

  // Absolute movement relative to stationary baseline
  float motionSignal =
    fabs(filteredMagnitude - baselineMagnitude);

  // State 0: user remains still
  if (systemState == BASELINE_CALIBRATION) {
    baselineSum += filteredMagnitude;
    baselineSamples++;

    if (currentTime - baselineStartTime >=
        BASELINE_TIME_MS) {
      baselineMagnitude =
        baselineSum / baselineSamples;

      systemState = MOVEMENT_CALIBRATION;
      resetMovementCycle();
    }
  }

  // State 1: first similar squats are used for calibration
  else if (systemState == MOVEMENT_CALIBRATION) {
    processMovement(
      motionSignal,
      CALIBRATION_START_GATE,
      CALIBRATION_END_GATE,
      currentTime
    );
  }

  // State 2: subsequent squats are counted
  else if (systemState == REP_COUNTING) {
    processMovement(
      motionSignal,
      highThreshold,
      lowThreshold,
      currentTime
    );
  }

  printPlotterData(
    rawMagnitude,
    filteredMagnitude,
    motionSignal
  );
}