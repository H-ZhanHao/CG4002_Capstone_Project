#pragma once

#include <Arduino.h>

enum CountingMode {
  PAUSE_MODE = 1,
  CONTINUOUS_MODE = 2
};

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

// Existing shared settings for the pause-based detector.
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

namespace GeneralConfig {
  constexpr int LED_PIN = LED_BUILTIN;
  constexpr int LED_ON = HIGH;
  constexpr int LED_OFF = LOW;

  constexpr unsigned long SAMPLE_INTERVAL_US = 20000;
  // Same USB/Bluetooth output cadence as the remote baseline.
  constexpr unsigned long OUTPUT_INTERVAL_MS = 100;

  constexpr float FILTER_ALPHA = 0.20f;

  constexpr unsigned long BASELINE_TIME_MS = 3000;
  constexpr int REQUIRED_STABLE_REPS = 2;

  constexpr unsigned long CALIBRATION_MIN_DURATION_MS = 700;
  constexpr unsigned long CALIBRATION_MAX_DURATION_MS = 6000;

  constexpr unsigned long QUIET_TIME_MS = 350;
  constexpr unsigned long MIN_REP_GAP_MS = 300;

  constexpr float POSITIONING_MOVEMENT_GYRO = 0.30f;
  constexpr float STATIONARY_MAX_GYRO = 0.15f;
  constexpr float STATIONARY_MAX_ACCEL_CHANGE = 0.05f;

  constexpr unsigned long CALIBRATION_LED_OFF_MS = 200;
  constexpr unsigned long REP_LED_DURATION_MS = 200;
}

namespace ModeConfig {
  // Default after power-on or hardware reset. USB/Bluetooth commands
  // MODE:PAUSE and MODE:CONTINUOUS can change it until the next reboot.
  constexpr CountingMode REQUESTED_MODE = PAUSE_MODE;
  static_assert(
    REQUESTED_MODE == PAUSE_MODE || REQUESTED_MODE == CONTINUOUS_MODE,
    "Select PAUSE_MODE or CONTINUOUS_MODE"
  );

  // Stillness gate used during continuous-counter calibration.
  constexpr float QUIET_GYRO_LIMIT = 0.20f;
}

namespace ContinuousConfig {
  constexpr float GYRO_FILTER_ALPHA = 0.25f;

  // Discover the dominant gyro axis from a moving window.
  constexpr unsigned long AXIS_WINDOW_MS = 1000;
  constexpr float MIN_AXIS_RMS = 0.40f;
  constexpr float MIN_AXIS_ENERGY_FRACTION = 0.55f;

  // Initial direction-detection gates, in rad/s.
  constexpr float INITIAL_START_GATE = 0.60f;
  constexpr float INITIAL_END_GATE = 0.18f;

  // Integrated angular travel per half-cycle, in radians.
  constexpr float MIN_HALF_ANGLE = 0.60f;

  constexpr unsigned long MIN_HALF_TIME_MS = 100;
  constexpr unsigned long END_SETTLE_MS = 60;

  constexpr unsigned long MIN_CYCLE_TIME_MS = 600;
  constexpr unsigned long MAX_CYCLE_TIME_MS = 6000;

  constexpr float PEAK_SIMILARITY = 0.25f;
  constexpr float ANGLE_SIMILARITY = 0.30f;
  constexpr float TIME_SIMILARITY = 0.30f;

  // Reject integration across a large sampling interruption.
  constexpr unsigned long MAX_SAMPLE_GAP_MS = 100;
}

namespace FallConfig {
  constexpr int BUZZER_PIN = D3;

  constexpr float FREE_FALL_THRESHOLD = 4.0f;
  constexpr unsigned long FREE_FALL_MIN_TIME_MS = 100;

  constexpr float IMPACT_THRESHOLD = 25.0f;
  constexpr unsigned long IMPACT_WINDOW_MS = 1000;

  constexpr unsigned long BUZZER_DURATION_MS = 3000;
}
