#pragma once

#include "SensorHistory.h"

namespace GenericPeakConfig {
  // Provisional movement gates, independent of all exercise counting presets.
  constexpr float ACCELERATION_GATE_MPS2 = 0.50f;
  constexpr float GYRO_GATE_RAD_S = 0.60f;
  constexpr float REARM_FRACTION = 0.45f;
  constexpr float PEAK_DROP_FRACTION = 0.25f;
  constexpr uint32_t MIN_ACTIVE_MS = 60;
  constexpr uint32_t FALL_CONFIRM_MS = 40;
  constexpr uint32_t REARM_MS = 80;
  constexpr uint32_t MIN_TRIGGER_GAP_MS = 1000;
  constexpr uint32_t MAX_SAMPLE_GAP_MS = 100;
  // Keep confirmation delay inside the buffer's extra three seconds of history.
  constexpr uint32_t MAX_CANDIDATE_MS = 2500;
  static_assert(MAX_CANDIDATE_MS < SensorHistoryConfig::RETENTION_MS - SensorHistoryConfig::WINDOW_MS,
                "Peak confirmation must fit retained history");
}

struct GenericPeakEvent {
  uint32_t peakMs = 0; // t0: sample time of the highest filtered motion score.
  uint32_t detectedMs = 0; // Later sample that confirmed the peak's decline.
  float accelerationMps2 = 0; // Median-filtered abs(|acceleration| - gravity).
  float gyroRadS = 0; // Median-filtered gyro magnitude at t0.
};

// A movement trigger, not an exercise classifier or repetition counter.
// No stillness calibration, allocation, radio, or clock synchronization.
class GenericPeakDetector {
 public:
  void reset();
  // True once per qualified excursion; event is unchanged on false.
  // Invalid readings / long gaps discard partial peaks, retaining the cooldown.
  bool update(const SensorSample& sample, GenericPeakEvent& event);

 private:
  void resetSignal();
  void disarm();
  float acceleration_[3] = {}, gyro_[3] = {};
  unsigned int filterCount_ = 0, filterIndex_ = 0;
  bool haveSample_ = false, haveTrigger_ = false;
  uint32_t lastSampleMs_ = 0, lastTriggerMs_ = 0;
  bool armed_ = true, quiet_ = false, candidate_ = false;
  bool aboveGate_ = false, qualified_ = false, falling_ = false;
  uint32_t quietSince_ = 0, candidateSince_ = 0, aboveSince_ = 0, fallingSince_ = 0;
  float peakScore_ = 0;
  GenericPeakEvent peak_;
};
