#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ExerciseConfig.h"

namespace SensorHistoryConfig {
  constexpr uint32_t WINDOW_MS = 3000;
  constexpr uint32_t RETENTION_MS = 8000; // Five seconds for a delayed trigger.
  static_assert(GeneralConfig::SAMPLE_INTERVAL_US >= 1000, "Use millisecond sample timestamps");
  constexpr size_t CAPACITY = RETENTION_MS * 1000 / GeneralConfig::SAMPLE_INTERVAL_US + 1;
  constexpr uint32_t MAX_GAP_MS = 2 * GeneralConfig::SAMPLE_INTERVAL_US / 1000;
}

struct SensorSample {
  uint32_t timestampMs; // Local uptime at the start of the successful sensor read.
  float acceleration[3]; // m/s2, including gravity; no exercise filtering.
  float gyro[3]; // rad/s, before bias correction.
};
static_assert(sizeof(SensorSample) == 28, "Keep sensor storage predictable");

enum class SensorWindowStatus {
  READY, SAMPLE_GAP, EMPTY, INSUFFICIENT_HISTORY, END_NOT_AVAILABLE, OUTPUT_TOO_SMALL
};

struct SensorWindowResult {
  SensorWindowStatus status = SensorWindowStatus::EMPTY;
  uint32_t startMs = 0, endMs = 0; // Requested inclusive interval, not sample endpoints.
  size_t count = 0;
  uint32_t firstMs = 0, lastMs = 0, maxGapMs = 0;
  bool copied() const {
    return status == SensorWindowStatus::READY || status == SensorWindowStatus::SAMPLE_GAP;
  }
};

const char* sensorWindowStatusName(SensorWindowStatus status);

// All calls belong to the node's main loop. No allocation or transport calls.
// Timestamps wrap modulo 2^32; comparisons require times less than 2^31 ms apart.
class SensorHistory {
 public:
  void clear();
  // Reject nonfinite values and duplicate/out-of-order timestamps.
  bool push(const SensorSample& sample);
  size_t size() const { return count_; }
  uint32_t oldestMs() const { return count_ ? at(0).timestampMs : 0; }
  uint32_t newestMs() const { return count_ ? at(count_ - 1).timestampMs : 0; }

  // Copy [endMs - WINDOW_MS, endMs] in time order. Never pad, resample or truncate.
  // Failure leaves destination unchanged. SAMPLE_GAP copies real samples and
  // flags a gap > MAX_GAP_MS (including the requested interval's edge gaps).
  SensorWindowResult extract(uint32_t endMs, SensorSample* destination, size_t capacity) const;

 private:
  SensorSample samples_[SensorHistoryConfig::CAPACITY] = {};
  size_t head_ = 0, count_ = 0;
  const SensorSample& at(size_t index) const {
    return samples_[(head_ + index) % SensorHistoryConfig::CAPACITY];
  }
};
