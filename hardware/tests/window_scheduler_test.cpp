#include <assert.h>
#include <stdio.h>
#include "WindowScheduler.h"

static SensorSample sample(uint32_t time) {
  SensorSample result = {};
  result.timestampMs = time;
  for (int axis = 0; axis < 3; ++axis) {
    result.acceleration[axis] = (float)(time % 10000) + axis;
    result.gyro[axis] = -(float)(time % 10000) - axis;
  }
  return result;
}

static void fill(SensorHistory& history, uint32_t from, uint32_t to) {
  for (uint32_t elapsed = 0; elapsed <= to - from; elapsed += 20)
    assert(history.push(sample(from + elapsed)));
}

static void checkWindow(const ScheduledWindow& frame, uint32_t session, uint32_t sequence,
                        uint32_t t0, uint32_t end) {
  assert(frame.info.session == session && frame.info.sequence == sequence);
  assert(frame.info.triggerMs == t0);
  const auto& window = frame.info.window;
  assert(window.status == SensorWindowStatus::READY && window.count == 151);
  assert(window.startMs == end - 3000 && window.endMs == end);
  for (size_t i = 0; i < window.count; ++i) {
    const uint32_t time = end - 3000 + (uint32_t)i * 20;
    assert(frame.samples[i].timestampMs == time);
    for (int axis = 0; axis < 3; ++axis) {
      assert(frame.samples[i].acceleration[axis] == sample(time).acceleration[axis]);
      assert(frame.samples[i].gyro[axis] == sample(time).gyro[axis]);
    }
  }
}

int main() {
  SensorHistory history;
  WindowScheduler scheduler;
  ScheduledWindow frame, snapshot;
  assert(scheduler.status().state == WindowStreamState::ARMED);
  assert(!scheduler.update(6000, history));
  frame.info.sequence = 99;
  assert(!scheduler.take(frame) && frame.info.sequence == 99);

  // A delayed peak still captures t0 first, then catches up on the original grid.
  fill(history, 0, 7500);
  assert(!scheduler.start(7520, 7500, history)); // Future anchor: no side effects.
  assert(scheduler.status().session == 0 && scheduler.status().queued == 0);
  assert(scheduler.start(5000, 7500, history));
  assert(scheduler.status().queued == 1);
  assert(scheduler.take(snapshot));
  checkWindow(snapshot, 1, 0, 5000, 5000);
  assert(!scheduler.start(7000, 7500, history)); // Later peak cannot move the anchor.
  scheduler.arm();
  assert(scheduler.status().state == WindowStreamState::RUNNING);
  assert(scheduler.update(7500, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 1, 5000, 6000);
  assert(scheduler.update(7500, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 2, 5000, 7000);
  assert(!scheduler.update(7500, history));

  // Service jitter never shifts endpoints or produces duplicate windows.
  uint32_t lastSample = 7500;
  for (uint32_t sequence = 3; sequence <= 20; ++sequence) {
    const uint32_t end = 5000 + sequence * 1000;
    fill(history, lastSample + 20, end + 120);
    lastSample = end + 120;
    assert(!scheduler.update(end - 1, history));
    assert(scheduler.update(end + (sequence % 3) * 37, history));
    assert(!scheduler.update(end + 120, history));
    assert(scheduler.take(frame));
    checkWindow(frame, 1, sequence, 5000, end);
  }
  // A copied first window survives all subsequent ring and queue reuse.
  checkWindow(snapshot, 1, 0, 5000, 5000);

  // A missing consumer never overwrites the first queued snapshot or stops sampling.
  scheduler.reset();
  history.clear();
  fill(history, 0, 5000);
  assert(scheduler.start(5000, 5000, history));
  for (uint32_t end = 6000; end <= 15000; end += 1000) {
    fill(history, end - 980, end);
    assert(scheduler.update(end, history));
  }
  assert(scheduler.status().queued == 3 && scheduler.status().dropped == 8);
  assert(!scheduler.lastPreparation().queued);
  assert(scheduler.status().nextEndMs == 16000 && history.newestMs() == 15000);
  for (uint32_t sequence = 0; sequence < 3; ++sequence) {
    assert(scheduler.take(frame));
    checkWindow(frame, 1, sequence, 5000, 5000 + sequence * 1000);
  }
  fill(history, 15020, 16000);
  assert(scheduler.update(16000, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 11, 5000, 16000); // Gaps in sequence expose dropped windows.

  // A stalled loop skips an unbounded backlog, then handles one recent slot per call.
  fill(history, 16020, 30000);
  assert(scheduler.update(30000, history));
  assert(scheduler.status().skipped == 11 && scheduler.status().queued == 1);
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 23, 5000, 28000);
  assert(scheduler.update(30000, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 24, 5000, 29000);
  assert(scheduler.update(30000, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 25, 5000, 30000);
  assert(!scheduler.update(30000, history));

  // Stop clears queued data and disarms; arm waits for a fresh trigger.
  fill(history, 30020, 31000);
  assert(scheduler.update(31000, history));
  scheduler.stop();
  assert(scheduler.status().state == WindowStreamState::STOPPED);
  assert(!scheduler.take(frame) && !scheduler.update(40000, history));
  scheduler.arm();
  assert(scheduler.status().state == WindowStreamState::ARMED && !scheduler.update(40000, history));
  assert(scheduler.start(31000, 31000, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 2, 0, 31000, 31000);
  scheduler.stop();
  assert(scheduler.start(31000, 31000, history)); // Explicit start can resume STOPPED.
  assert(scheduler.status().session == 3);

  // Startup errors are explicit records; later windows recover without a new peak.
  scheduler.reset();
  history.clear();
  assert(scheduler.start(0, 0, history));
  assert(scheduler.take(frame) && frame.info.window.status == SensorWindowStatus::EMPTY);
  fill(history, 0, 1000);
  assert(scheduler.update(1000, history));
  assert(scheduler.take(frame) && frame.info.window.status == SensorWindowStatus::INSUFFICIENT_HISTORY);
  assert(frame.info.sequence == 1 && frame.info.window.count == 0);
  for (uint32_t end = 2000; end <= 5000; end += 1000) {
    fill(history, end - 980, end);
    assert(scheduler.update(end, history));
    assert(scheduler.take(frame));
  }
  checkWindow(frame, 1, 5, 0, 5000);

  // Endpoints between samples wait briefly, then keep exact requested boundaries.
  fill(history, 5020, 5980);
  assert(!scheduler.update(6000, history));
  assert(!scheduler.update(6099, history));
  assert(history.push(sample(6100)));
  assert(scheduler.update(6100, history));
  assert(scheduler.take(frame));
  assert(frame.info.window.endMs == 6000 && frame.info.window.lastMs == 5980);
  assert(frame.info.window.count == 150);

  // Failed reads do not hold the scheduler forever or fabricate endpoint samples.
  assert(!scheduler.update(7000, history));
  assert(scheduler.update(7100, history));
  frame.samples[0].timestampMs = 123;
  assert(scheduler.take(frame));
  assert(frame.info.sequence == 7 && frame.info.window.status == SensorWindowStatus::END_NOT_AVAILABLE);
  assert(frame.info.window.count == 0 && frame.samples[0].timestampMs == 123);
  fill(history, 7120, 8000);
  assert(scheduler.update(8000, history));
  assert(scheduler.take(frame) && frame.info.window.status == SensorWindowStatus::SAMPLE_GAP);
  assert(frame.info.window.maxGapMs == 1020);

  // Off-grid t0 stays off-grid even as new samples arrive on the 20 ms cadence.
  scheduler.reset();
  history.clear();
  fill(history, 0, 8020);
  assert(scheduler.start(7011, 8020, history));
  assert(scheduler.take(frame));
  assert(frame.info.window.startMs == 4011 && frame.info.window.firstMs == 4020);
  assert(frame.info.window.endMs == 7011 && frame.info.window.lastMs == 7000);
  assert(scheduler.update(8020, history));
  assert(scheduler.take(frame));
  assert(frame.info.window.startMs == 5011 && frame.info.window.endMs == 8011);
  assert(frame.info.window.lastMs == 8000 && frame.info.window.count == 150);

  // Denser input fails explicitly rather than overflowing/truncating a snapshot.
  scheduler.reset();
  history.clear();
  for (uint32_t time = 0; time <= 5000; time += 10) {
    assert(history.push(sample(time)));
    if (time >= 2000 && time < 2020) assert(history.push(sample(time + 5)));
  }
  assert(scheduler.start(5000, 5000, history));
  frame.samples[0].timestampMs = 123;
  assert(scheduler.take(frame));
  assert(frame.info.window.status == SensorWindowStatus::OUTPUT_TOO_SMALL);
  assert(frame.info.window.count == 303 && !frame.info.window.copied());
  assert(frame.samples[0].timestampMs == 123);

  // Local millisecond rollover does not change interval ordering or readiness.
  scheduler.reset();
  history.clear();
  const uint32_t origin = UINT32_MAX - 5499;
  fill(history, origin, origin + 5000);
  assert(scheduler.start(origin + 5000, origin + 5000, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 0, origin + 5000, origin + 5000);
  fill(history, origin + 5020, origin + 6000);
  assert(!scheduler.update(origin + 5999, history));
  assert(scheduler.update(origin + 6000, history));
  assert(scheduler.take(frame));
  checkWindow(frame, 1, 1, origin + 5000, origin + 6000);
  scheduler.reset();
  assert(!scheduler.take(frame) && scheduler.status().state == WindowStreamState::ARMED);
  puts("Window scheduler anchoring, snapshots, overflow, catch-up, controls, gaps and rollover tests passed");
}
