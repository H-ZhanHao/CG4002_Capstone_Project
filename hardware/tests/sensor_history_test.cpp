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

  // Both endpoints are included. Five seconds at exactly 50 Hz has 251 samples.
  for (uint32_t t = 0; t < 5000; t += 20) assert(history.push(sample(t, (float)t)));
  assert(history.extract(4980, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);
  assert(history.extract(5000, window, capacity).status == SensorWindowStatus::END_NOT_AVAILABLE);
  assert(window[0].timestampMs == 123);
  assert(history.push(sample(5000, 5000.0f)));
  auto result = history.extract(5000, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.copied());
  assert(result.count == 251 && result.startMs == 0 && result.endMs == 5000);
  assert(result.firstMs == 0 && result.lastMs == 5000 && result.maxGapMs == 20);
  for (size_t i = 0; i < result.count; ++i) {
    assert(window[i].timestampMs == i * 20);
    for (int axis = 0; axis < 3; ++axis) {
      assert(window[i].acceleration[axis] == (float)(i * 20 + axis));
      assert(window[i].gyro[axis] == -(float)(i * 20 + axis));
    }
  }
  window[0] = sample(123);
  result = history.extract(5000, window, 250);
  assert(result.status == SensorWindowStatus::OUTPUT_TOO_SMALL && result.count == 251);
  assert(!result.copied() && window[0].timestampMs == 123);
  assert(history.extract(5000, nullptr, capacity).status == SensorWindowStatus::OUTPUT_TOO_SMALL);

  // Repeated physical ring wrap; a trigger delayed by three seconds still fits.
  for (uint32_t t = 5020; t <= 40000; t += 20) assert(history.push(sample(t)));
  assert(history.size() == capacity && history.oldestMs() == 32000 && history.newestMs() == 40000);
  result = history.extract(37000, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.count == 251);
  assert(window[0].timestampMs == 32000 && window[250].timestampMs == 37000);
  assert(history.extract(36999, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);
  assert(history.extract(40001, window, capacity).status == SensorWindowStatus::END_NOT_AVAILABLE);
  // New writes must not mutate a previously copied snapshot.
  for (uint32_t t = 40020; t <= 50000; t += 20) assert(history.push(sample(t)));
  assert(window[0].timestampMs == 32000 && window[250].timestampMs == 37000);
  assert(history.extract(37000, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);

  // Invalid samples and bad timestamps cannot replace good history.
  const size_t before = history.size();
  SensorSample invalid = sample(50020);
  invalid.acceleration[1] = NAN;
  assert(!history.push(invalid));
  invalid = sample(50020);
  invalid.gyro[2] = INFINITY;
  assert(!history.push(invalid));
  assert(!history.push(sample(50000)) && !history.push(sample(49900)));
  assert(history.size() == before && history.newestMs() == 50000);

  // Sparse or failed reads remain gaps; extraction never manufactures samples.
  history.clear();
  for (uint32_t t = 0; t <= 8000; t += 20) {
    if (t >= 4000 && t < 4300) continue;
    assert(history.push(sample(t)));
  }
  result = history.extract(8000, window, capacity);
  assert(result.status == SensorWindowStatus::SAMPLE_GAP && result.copied());
  assert(result.count == 236 && result.maxGapMs == 320);
  for (size_t i = 0; i < result.count; ++i)
    assert(window[i].timestampMs < 4000 || window[i].timestampMs >= 4300);
  assert(history.push(sample(20000))); // Long outage expires old history.
  assert(history.size() == 1);
  assert(history.extract(20000, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);

  // A window with an endpoint between samples must stay inside its timestamps.
  history.clear();
  for (uint32_t t = 0; t <= 8000; t += 20) assert(history.push(sample(t)));
  result = history.extract(7011, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.count == 250);
  assert(result.startMs == 2011 && result.firstMs == 2020 && result.lastMs == 7000);
  assert(result.maxGapMs == 20);

  // Detect an edge gap even when it straddles the requested start/end boundary.
  history.clear();
  assert(history.push(sample(0)));
  for (uint32_t t = 300; t <= 6000; t += 20) assert(history.push(sample(t)));
  result = history.extract(5100, window, capacity);
  assert(result.status == SensorWindowStatus::SAMPLE_GAP && result.maxGapMs == 200);
  history.clear();
  for (uint32_t t = 0; t <= 5100; t += 20) assert(history.push(sample(t)));
  assert(history.push(sample(5500)));
  result = history.extract(5400, window, capacity);
  assert(result.status == SensorWindowStatus::SAMPLE_GAP && result.maxGapMs == 300);

  // millis() rollover is distinct from the circular array wrapping.
  history.clear();
  const uint32_t origin = UINT32_MAX - 2999;
  for (uint32_t elapsed = 0; elapsed <= 10000; elapsed += 20)
    assert(history.push(sample(origin + elapsed)));
  result = history.extract(origin + 7000, window, capacity);
  assert(result.status == SensorWindowStatus::READY && result.count == 251);
  assert(result.firstMs == origin + 2000 && result.lastMs == origin + 7000);
  for (size_t i = 1; i < result.count; ++i)
    assert((uint32_t)(window[i].timestampMs - window[i - 1].timestampMs) == 20);

  // Sample count alone cannot prove five seconds of coverage.
  history.clear();
  for (uint32_t t = 0; t < 1000; ++t) assert(history.push(sample(t)));
  assert(history.size() == capacity);
  assert(history.extract(999, window, capacity).status == SensorWindowStatus::INSUFFICIENT_HISTORY);

  // Compare variable-interval windows against an independent linear reference.
  history.clear();
  std::vector<SensorSample> reference;
  uint32_t t = 0;
  for (int i = 0; i < 2000; ++i) {
    t += 20 + (i % 7);
    reference.push_back(sample(t, (float)i));
    assert(history.push(reference.back()));
    if (t < 10000) continue;
    const uint32_t end = t - 1703;
    result = history.extract(end, window, capacity);
    assert(result.status == SensorWindowStatus::READY);
    size_t index = 0;
    for (const SensorSample& expected : reference) {
      if (expected.timestampMs < end - 5000 || expected.timestampMs > end) continue;
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
