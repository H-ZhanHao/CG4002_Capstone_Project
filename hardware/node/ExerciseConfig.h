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
// EXERCISE SETTINGS STRUCTURE
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
// SQUAT SETTINGS
// Sensor position: Hip
// Signal: Acceleration magnitude
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
// PUSH-UP SETTINGS
// Sensor position: Chest
// Signal: Acceleration magnitude
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

// ======================================================
// RETURN SETTINGS FOR SELECTED EXERCISE
// ======================================================

inline ExerciseSettings getExerciseSettings(
  ExerciseType exercise
) {
  switch (exercise) {
    case SQUAT:
      return getSquatSettings();

    case PUSH_UP:
      return getPushUpSettings();

    default:
      // Safety fallback
      return getSquatSettings();
  }
}

// ======================================================
// GENERAL SETTINGS FOR ALL EXERCISES
// ======================================================

namespace GeneralConfig {
  // Built in esp32 led (D9)
  constexpr int LED_PIN = LED_BUILTIN;

  // 50 Hz sampling
  constexpr unsigned long SAMPLE_INTERVAL_US =
    20000;

  // Exponential moving-average filter
  constexpr float FILTER_ALPHA = 0.20;

  // Stationary baseline calibration duration
  constexpr unsigned long BASELINE_TIME_MS =
    2000;

  // Number of mutually similar calibration movements
  constexpr int REQUIRED_STABLE_REPS = 3;

  // Broad duration limits during calibration
  constexpr unsigned long
    CALIBRATION_MIN_DURATION_MS = 700;

  constexpr unsigned long
    CALIBRATION_MAX_DURATION_MS = 6000;

  // Time near baseline required to complete a movement
  constexpr unsigned long QUIET_TIME_MS = 350;

  // Minimum gap before another movement can begin
  constexpr unsigned long MIN_REP_GAP_MS = 300;
}