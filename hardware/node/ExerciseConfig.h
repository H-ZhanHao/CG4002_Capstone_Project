#pragma once

#include <stdint.h>
#include "NodeTypes.h"

namespace NodeConfig {
  // Roles are selected by the PlatformIO environment; no role edits needed here.

  // Enter each board's Wi-Fi STA MAC, printed on its USB monitor at startup.
  // Bluetooth MAC addresses are different. Zero addresses disable the peer link
  // until configured. Activation requires the other board's acknowledgement.
  constexpr uint8_t HIP_MAC[6] = {0x38, 0x18, 0x2B, 0x19, 0x55, 0xBC};
  constexpr uint8_t WRIST_MAC[6] = {0x38, 0x18, 0x2B, 0x18, 0x4E, 0xB8};
  constexpr uint8_t RADIO_CHANNEL = 6; // Must match on both boards.
  static_assert(RADIO_CHANNEL >= 1 && RADIO_CHANNEL <= 13, "Use a 2.4 GHz Wi-Fi channel from 1 to 13");
  constexpr const char* HIP_BLUETOOTH_NAME = "ExerciseHip";
  constexpr const char* WRIST_BLUETOOTH_NAME = "ExerciseWrist";
}

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

  // Allow moderate peak variation between otherwise similar movements.
  settings.peakSimilarityTolerance = 0.30f;
  settings.durationSimilarityTolerance = 0.25f;

  settings.highThresholdFactor = 0.55f;
  settings.lowThresholdFactor = 0.20f;
  settings.requiredPeakFactor = 0.70f;

  // Keep the original duration guard against short movement fragments.
  settings.minimumDurationFactor = 0.60f;
  settings.maximumDurationFactor = 1.60f;

  return settings;
}

namespace GeneralConfig {
  // Original FireBeetle ESP32 GPIO numbers (not printed D-pin numbers).
  constexpr int LED_PIN = 2;
  constexpr int LED_ON = 1;
  constexpr int LED_OFF = 0;
  constexpr int I2C_SDA_PIN = 21;
  constexpr int I2C_SCL_PIN = 22;
  constexpr uint8_t MPU6050_ADDRESS = 0x68;

  constexpr unsigned long SAMPLE_INTERVAL_US = 20000;
  // Same USB/Bluetooth output cadence as the remote baseline.
  constexpr unsigned long OUTPUT_INTERVAL_MS = 100;

  constexpr float FILTER_ALPHA = 0.20f;

  // Nominal sensor references: preset startup never waits for stillness.
  // These are assumptions, not measured offsets; verify with the worn sensors.
  constexpr float GRAVITY_MPS2 = 9.81f;
  constexpr float GYRO_BIAS_RAD_S[3] = {0.0f, 0.0f, 0.0f};

  // Retained for the legacy calibration API/tests, not firmware startup.
  constexpr unsigned long BASELINE_TIME_MS = 3000;
  constexpr int REQUIRED_STABLE_REPS = 2;

  constexpr unsigned long CALIBRATION_MIN_DURATION_MS = 700;
  constexpr unsigned long CALIBRATION_MAX_DURATION_MS = 6000;

  constexpr unsigned long QUIET_TIME_MS = 350;
  // Includes the quiet period that completes a movement.
  constexpr unsigned long MIN_REP_GAP_MS = 300;

  constexpr unsigned long CALIBRATION_LED_OFF_MS = 200;
  // Mirrored ready/rep pulse durations are shared in NodeSession.h: LedTiming.
}

namespace ModeConfig {
  // Exercise defaults live in ExercisePresets.h; MODE commands override them.
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
  constexpr int BUZZER_PIN = 26; // FireBeetle D3

  constexpr float FREE_FALL_THRESHOLD = 4.0f;
  constexpr unsigned long FREE_FALL_MIN_TIME_MS = 100;

  constexpr float IMPACT_THRESHOLD = 25.0f;
  constexpr unsigned long IMPACT_WINDOW_MS = 1000;

  constexpr unsigned long BUZZER_DURATION_MS = 3000;
}
