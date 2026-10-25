#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <vector>
#include "SensorHistory.h"

static SensorSample sample(uint32_t time, float value = 1.0f) {
  SensorSample result = {};
  result.timestampMs = time;
  for (int i = 0; i < 3; ++i) {
    result.acceleration[i] = value + i;
    result.gyro[i] = -value - i;
  }
  return result;
}

int main() {
  SensorHistory history;
  SensorSample window[SensorHistoryConfig::CAPACITY];
  const size_t capacity = SensorHistoryConfig::CAPACITY;
  window[0] = sample(123);
  assert(history.extract(0, window, capacity).status == SensorWindowStatus::EMPTY);
  assert(window[0].timestampMs == 123);

  // Both endpoints are included. Three seconds at exactly 100 Hz has 301 samples.
  for (uint32_t t = 0; t < 3000; t += 10) assert(history.push(sample(t, (float)t)));
  assert(history.extract(2990, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);
  assert(history.extract(3000, window, capacity).status == SensorWindowStatus::END_NOT_AVAILABLE);
  assert(window[0].timestampMs == 123);
  assert(history.push(sample(3000, 3000.0f)));
  auto result = history.extract(3000, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.copied());
  assert(result.count == 301 && result.startMs == 0 && result.endMs == 3000);
  assert(result.firstMs == 0 && result.lastMs == 3000 && result.maxGapMs == 10);
  for (size_t i = 0; i < result.count; ++i) {
    assert(window[i].timestampMs == i * 10);
    for (int axis = 0; axis < 3; ++axis) {
      assert(window[i].acceleration[axis] == (float)(i * 10 + axis));
      assert(window[i].gyro[axis] == -(float)(i * 10 + axis));
    }
  }
  window[0] = sample(123);
  result = history.extract(3000, window, 300);
  assert(result.status == SensorWindowStatus::OUTPUT_TOO_SMALL && result.count == 301);
  assert(!result.copied() && window[0].timestampMs == 123);
  assert(history.extract(3000, nullptr, capacity).status == SensorWindowStatus::OUTPUT_TOO_SMALL);

  // Repeated physical ring wrap; a trigger delayed by five seconds still fits.
  for (uint32_t t = 3010; t <= 40000; t += 10) assert(history.push(sample(t)));
  assert(history.size() == capacity && history.oldestMs() == 32000 && history.newestMs() == 40000);
  result = history.extract(35000, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.count == 301);
  assert(window[0].timestampMs == 32000 && window[300].timestampMs == 35000);
  assert(history.extract(34999, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);
  assert(history.extract(40001, window, capacity).status == SensorWindowStatus::END_NOT_AVAILABLE);
  // New writes must not mutate a previously copied snapshot.
  for (uint32_t t = 40010; t <= 50000; t += 10) assert(history.push(sample(t)));
  assert(window[0].timestampMs == 32000 && window[300].timestampMs == 35000);
  assert(history.extract(35000, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);

  // Invalid samples and bad timestamps cannot replace good history.
  const size_t before = history.size();
  SensorSample invalid = sample(50010);
  invalid.acceleration[1] = NAN;
  assert(!history.push(invalid));
  invalid = sample(50010);
  invalid.gyro[2] = INFINITY;
  assert(!history.push(invalid));
  assert(!history.push(sample(50000)) && !history.push(sample(49900)));
  assert(history.size() == before && history.newestMs() == 50000);

  // Sparse or failed reads remain gaps; extraction never manufactures samples.
  history.clear();
  for (uint32_t t = 0; t <= 8000; t += 10) {
    if (t >= 6000 && t < 6300) continue;
    assert(history.push(sample(t)));
  }
  result = history.extract(8000, window, capacity);
  assert(result.status == SensorWindowStatus::SAMPLE_GAP && result.copied());
  assert(result.count == 271 && result.maxGapMs == 310);
  for (size_t i = 0; i < result.count; ++i)
    assert(window[i].timestampMs < 6000 || window[i].timestampMs >= 6300);
  assert(history.push(sample(20000))); // Long outage expires old history.
  assert(history.size() == 1);
  assert(history.extract(20000, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);

  // A window with an endpoint between samples must stay inside its timestamps.
  history.clear();
  for (uint32_t t = 0; t <= 8000; t += 10) assert(history.push(sample(t)));
  result = history.extract(7011, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.count == 300);
  assert(result.startMs == 4011 && result.firstMs == 4020 && result.lastMs == 7010);
  assert(result.maxGapMs == 10);

  // Detect an edge gap even when it straddles the requested start/end boundary.
  history.clear();
  assert(history.push(sample(0)));
  for (uint32_t t = 300; t <= 6000; t += 10) assert(history.push(sample(t)));
  result = history.extract(3100, window, capacity);
  assert(result.status == SensorWindowStatus::SAMPLE_GAP && result.maxGapMs == 200);
  history.clear();
  for (uint32_t t = 0; t <= 5100; t += 10) assert(history.push(sample(t)));
  assert(history.push(sample(5500)));
  result = history.extract(5400, window, capacity);
  assert(result.status == SensorWindowStatus::SAMPLE_GAP && result.maxGapMs == 300);

  // millis() rollover is distinct from the circular array wrapping.
  history.clear();
  const uint32_t origin = UINT32_MAX - 5499;
  for (uint32_t elapsed = 0; elapsed <= 10000; elapsed += 10)
    assert(history.push(sample(origin + elapsed)));
  result = history.extract(origin + 7000, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.count == 301);
  assert(result.firstMs == origin + 4000 && result.lastMs == origin + 7000);
  for (size_t i = 1; i < result.count; ++i)
    assert((uint32_t)(window[i].timestampMs - window[i - 1].timestampMs) == 10);

  // Sample count alone cannot prove three seconds of coverage.
  history.clear();
  for (uint32_t t = 0; t < 1000; ++t) assert(history.push(sample(t)));
  assert(history.size() == capacity);
  assert(history.extract(999, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);

  // Compare variable-interval windows against an independent linear reference.
  history.clear();
  std::vector<SensorSample> reference;
  uint32_t t = 0;
  for (int i = 0; i < 2000; ++i) {
    t += 10 + (i % 7);
    reference.push_back(sample(t, (float)i));
    assert(history.push(reference.back()));
    if (t < 10000) continue;
    const uint32_t end = t - 1703;
    result = history.extract(end, window, capacity);
    assert(result.status == SensorWindowStatus::READY);
    size_t index = 0;
    for (const SensorSample& expected : reference) {
      if (expected.timestampMs < end - 3000 || expected.timestampMs > end) continue;
      assert(index < result.count);
      assert(window[index].timestampMs == expected.timestampMs);
      assert(window[index].acceleration[0] == expected.acceleration[0]);
      ++index;
    }
    assert(index == result.count);
  }
  history.clear();
  assert(history.size() == 0 && history.extract(t, window, capacity).status == SensorWindowStatus::EMPTY);
  puts("Sensor history boundaries, retention, gaps, snapshots, jitter and rollover tests passed");
}
