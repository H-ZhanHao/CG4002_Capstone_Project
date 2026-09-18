#pragma once

#include <Arduino.h>

// ======================================================
// EXERCISE LABELS
// These describe recognition results.
// They do not select the rep-counting settings.
// ======================================================

enum ExerciseType {
  UNKNOWN_EXERCISE = -1,
  SQUAT = 0,
  PUSH_UP = 1,
  SIT_UP = 2,
  JUMPING_JACK = 3,
  LUNGE = 4,
  LEG_RAISE = 5
};

// ======================================================
// SHARED REP-COUNTING SETTINGS
// Compatible with your current RepCounter class.
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

// One provisional profile for every exercise.
// Recognition never changes these settings in this version.
//
// Acceleration thresholds are in m/s².
// Validate these gates using your actual sensor placement.
inline ExerciseSettings getGenericSettings() {
  ExerciseSettings settings;

  settings.calibrationStartGate = 0.50f;
  settings.calibrationEndGate = 0.20f;
  settings.minimumCalibrationPeak = 0.70f;

  settings.peakSimilarityTolerance = 0.20f;
  settings.durationSimilarityTolerance = 0.25f;

  settings.highThresholdFactor = 0.55f;
  settings.lowThresholdFactor = 0.20f;
  settings.requiredPeakFactor = 0.70f;

  settings.minimumDurationFactor = 0.60f;
  settings.maximumDurationFactor = 1.60f;

  return settings;
}

// ======================================================
// GENERAL SETTINGS
// ======================================================

namespace GeneralConfig {
  constexpr int LED_PIN = LED_BUILTIN;

  constexpr int LED_ON = HIGH;
  constexpr int LED_OFF = LOW;

  constexpr unsigned long SAMPLE_INTERVAL_US = 20000;

  // USB and Bluetooth output at approximately 10 Hz.
  constexpr unsigned long OUTPUT_INTERVAL_MS = 100;

  constexpr float FILTER_ALPHA = 0.20f;

  constexpr unsigned long BASELINE_TIME_MS = 3000;
  constexpr int REQUIRED_STABLE_REPS = 2;

  constexpr unsigned long CALIBRATION_MIN_DURATION_MS = 700;
  constexpr unsigned long CALIBRATION_MAX_DURATION_MS = 6000;

  constexpr unsigned long QUIET_TIME_MS = 350;
  constexpr unsigned long MIN_REP_GAP_MS = 300;

  // Gyroscope magnitudes: rad/s.
  constexpr float POSITIONING_MOVEMENT_GYRO = 0.30f;
  constexpr float STATIONARY_MAX_GYRO = 0.15f;

  // Successive filtered acceleration-magnitude change: m/s².
  constexpr float STATIONARY_MAX_ACCEL_CHANGE = 0.05f;

  constexpr unsigned long CALIBRATION_LED_OFF_MS = 200;
  constexpr unsigned long REP_LED_DURATION_MS = 200;
}

// ======================================================
// FALL DETECTION
// ======================================================

namespace FallConfig {
  constexpr int BUZZER_PIN = D3;

  constexpr float FREE_FALL_THRESHOLD = 4.0f;
  constexpr unsigned long FREE_FALL_MIN_TIME_MS = 100;

  constexpr float IMPACT_THRESHOLD = 25.0f;
  constexpr unsigned long IMPACT_WINDOW_MS = 1000;

  constexpr unsigned long BUZZER_DURATION_MS = 3000;
}