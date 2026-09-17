#pragma once

#include <Arduino.h>

// ======================================================
// EXERCISE TYPES
// ======================================================

enum ExerciseType {
  SQUAT = 0,
  PUSH_UP = 1
};

// ======================================================
// EXERCISE SETTINGS
// ======================================================

struct ExerciseSettings {
  float calibrationStartGate;
  float calibrationEndGate;
  float minimumCalibrationPeak;

  float peakSimilarityTolerance;
  float durationSimilarityTolerance;

  float highThresholdFactor;
  float lowThresholdFactor;
  float requiredPeakFactor;

  float minimumDurationFactor;
  float maximumDurationFactor;
};

// ======================================================
// SQUAT — HIP NODE
// ======================================================

inline ExerciseSettings getSquatSettings() {
  ExerciseSettings settings;

  settings.calibrationStartGate = 0.80;
  settings.calibrationEndGate = 0.30;
  settings.minimumCalibrationPeak = 1.00;

  settings.peakSimilarityTolerance = 0.20;
  settings.durationSimilarityTolerance = 0.25;

  settings.highThresholdFactor = 0.55;
  settings.lowThresholdFactor = 0.20;
  settings.requiredPeakFactor = 0.70;

  settings.minimumDurationFactor = 0.60;
  settings.maximumDurationFactor = 1.60;

  return settings;
}

// ======================================================
// PUSH-UP — CHEST NODE
// These values are not validated for wrist placement.
// ======================================================

inline ExerciseSettings getPushUpSettings() {
  ExerciseSettings settings;

  settings.calibrationStartGate = 0.50;
  settings.calibrationEndGate = 0.20;
  settings.minimumCalibrationPeak = 0.70;

  settings.peakSimilarityTolerance = 0.20;
  settings.durationSimilarityTolerance = 0.25;

  settings.highThresholdFactor = 0.55;
  settings.lowThresholdFactor = 0.20;
  settings.requiredPeakFactor = 0.70;

  settings.minimumDurationFactor = 0.60;
  settings.maximumDurationFactor = 1.60;

  return settings;
}

inline ExerciseSettings getExerciseSettings(
  ExerciseType exercise
) {
  switch (exercise) {
    case PUSH_UP:
      return getPushUpSettings();

    case SQUAT:
    default:
      return getSquatSettings();
  }
}

// ======================================================
// GENERAL CONFIGURATION
// ======================================================

namespace GeneralConfig {
  constexpr int LED_PIN = LED_BUILTIN;

  // Assumes HIGH turns the LED on.
  constexpr int LED_ON = HIGH;
  constexpr int LED_OFF = LOW;

  // Target sample interval: 50 Hz.
  constexpr unsigned long SAMPLE_INTERVAL_US = 20000;

  constexpr float FILTER_ALPHA = 0.20;

  // Three continuous seconds of detected stillness.
  constexpr unsigned long BASELINE_TIME_MS = 3000;

  // Requires the two-rep-compatible RepCounter.cpp.
  constexpr int REQUIRED_STABLE_REPS = 2;

  constexpr unsigned long CALIBRATION_MIN_DURATION_MS = 700;
  constexpr unsigned long CALIBRATION_MAX_DURATION_MS = 6000;

  constexpr unsigned long QUIET_TIME_MS = 350;
  constexpr unsigned long MIN_REP_GAP_MS = 300;

  // Gyroscope magnitudes are in rad/s.
  // Rotate/move the node after reset to arm preparation.
  constexpr float POSITIONING_MOVEMENT_GYRO = 0.30;
  constexpr float STATIONARY_MAX_GYRO = 0.15;

  // Maximum change in filtered acceleration magnitude
  // between successive samples, in m/s².
  constexpr float STATIONARY_MAX_ACCEL_CHANGE = 0.05;

  constexpr int CALIBRATION_READY_BLINKS = 3;
  constexpr unsigned long
    CALIBRATION_READY_BLINK_INTERVAL_MS = 200;

  constexpr unsigned long REP_LED_DURATION_MS = 200;
}

// ======================================================
// FALL DETECTION CONFIGURATION
// Prototype thresholds — require experimental validation.
// ======================================================

namespace FallConfig {
  // Keep this consistent with your actual wiring.
  constexpr int BUZZER_PIN = D3;

  // Acceleration magnitude in m/s².
  constexpr float FREE_FALL_THRESHOLD = 4.0;
  constexpr unsigned long FREE_FALL_MIN_TIME_MS = 100;

  constexpr float IMPACT_THRESHOLD = 25.0;
  constexpr unsigned long IMPACT_WINDOW_MS = 1000;

  constexpr unsigned long BUZZER_DURATION_MS = 3000;
}