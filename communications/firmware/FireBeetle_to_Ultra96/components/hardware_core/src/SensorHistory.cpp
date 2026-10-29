#include "SensorHistory.h"
#include <math.h>

namespace { constexpr uint32_t HALF_RANGE = 0x80000000u; }

void SensorHistory::clear() { head_ = count_ = 0; }

bool SensorHistory::push(const SensorSample& sample) {
  for (int axis = 0; axis < 3; ++axis) {
    if (!isfinite(sample.acceleration[axis]) || !isfinite(sample.gyro[axis])) return false;
  }
  if (count_) {
    const uint32_t elapsed = sample.timestampMs - newestMs();
    if (!elapsed || elapsed >= HALF_RANGE) return false;
  }
  while (count_ && sample.timestampMs - oldestMs() > SensorHistoryConfig::RETENTION_MS) {
    head_ = (head_ + 1) % SensorHistoryConfig::CAPACITY;
    --count_;
  }
  if (count_ == SensorHistoryConfig::CAPACITY) {
    head_ = (head_ + 1) % SensorHistoryConfig::CAPACITY;
    --count_;
  }
  samples_[(head_ + count_) % SensorHistoryConfig::CAPACITY] = sample;
  ++count_;
  return true;
}

SensorWindowResult SensorHistory::extract(uint32_t endMs, SensorSample* destination,
                                         size_t capacity) const {
  SensorWindowResult result;
  result.endMs = endMs;
  result.startMs = endMs - SensorHistoryConfig::WINDOW_MS;
  if (!count_) return result;
  const uint32_t afterNewest = endMs - newestMs();
  if (afterNewest && afterNewest < HALF_RANGE) {
    result.status = SensorWindowStatus::END_NOT_AVAILABLE;
    return result;
  }
  if (result.startMs - oldestMs() >= HALF_RANGE) {
    result.status = SensorWindowStatus::INSUFFICIENT_HISTORY;
    return result;
  }

  uint32_t previousMs = result.startMs;
  for (size_t i = 0; i < count_; ++i) {
    const SensorSample& sample = at(i);
    if (endMs - sample.timestampMs > SensorHistoryConfig::WINDOW_MS) continue;
    if (!result.count) result.firstMs = sample.timestampMs;
    ++result.count;
    const uint32_t gap = sample.timestampMs - previousMs;
    if (gap > result.maxGapMs) result.maxGapMs = gap;
    previousMs = result.lastMs = sample.timestampMs;
  }
  const uint32_t endGap = endMs - previousMs;
  if (endGap > result.maxGapMs) result.maxGapMs = endGap;
  if (!result.count) {
    result.status = SensorWindowStatus::INSUFFICIENT_HISTORY;
    return result;
  }
  if (!destination || capacity < result.count) {
    result.status = SensorWindowStatus::OUTPUT_TOO_SMALL;
    return result;
  }
  size_t copied = 0;
  for (size_t i = 0; i < count_; ++i) {
    const SensorSample& sample = at(i);
    if (endMs - sample.timestampMs <= SensorHistoryConfig::WINDOW_MS)
      destination[copied++] = sample;
  }
  result.status = result.maxGapMs > SensorHistoryConfig::MAX_GAP_MS
    ? SensorWindowStatus::SAMPLE_GAP : SensorWindowStatus::READY;
  return result;
}

const char* sensorWindowStatusName(SensorWindowStatus status) {
  switch (status) {
    case SensorWindowStatus::READY: return "READY";
    case SensorWindowStatus::SAMPLE_GAP: return "SAMPLE_GAP";
    case SensorWindowStatus::EMPTY: return "EMPTY";
    case SensorWindowStatus::INSUFFICIENT_HISTORY: return "INSUFFICIENT_HISTORY";
    case SensorWindowStatus::END_NOT_AVAILABLE: return "END_NOT_AVAILABLE";
    case SensorWindowStatus::OUTPUT_TOO_SMALL: return "OUTPUT_TOO_SMALL";
  }
  return "UNKNOWN";
}
