#include "WindowScheduler.h"
#include <string.h>

namespace { constexpr uint32_t HALF_RANGE = 0x80000000u; }

void WindowScheduler::reset() {
  status_ = WindowStreamStatus{};
  last_ = WindowPreparation{};
  head_ = 0;
}

void WindowScheduler::arm() {
  if (status_.state != WindowStreamState::RUNNING) status_.state = WindowStreamState::ARMED;
}

void WindowScheduler::stop() {
  status_.state = WindowStreamState::STOPPED;
  status_.queued = 0;
  head_ = 0;
}

bool WindowScheduler::start(uint32_t t0, uint32_t now, const SensorHistory& history,
                            uint32_t coordinator, uint32_t sharedSession) {
  if ((!coordinator) != (!sharedSession)) return false;
  if (status_.state == WindowStreamState::RUNNING || now - t0 >= HALF_RANGE) return false;
  uint32_t session = status_.session + 1;
  if (!session) ++session;
  status_ = WindowStreamStatus{};
  status_.state = WindowStreamState::RUNNING;
  status_.coordinator = coordinator;
  status_.session = coordinator ? sharedSession : session;
  status_.triggerMs = t0;
  status_.nextEndMs = t0 + WindowSchedulerConfig::STEP_MS;
  status_.nextSequence = 1;
  head_ = 0;
  prepare(t0, 0, history);
  return true;
}

void WindowScheduler::prepare(uint32_t endMs, uint32_t sequence, const SensorHistory& history) {
  last_ = WindowPreparation{};
  last_.info.coordinator = status_.coordinator;
  last_.info.session = status_.session;
  last_.info.sequence = sequence;
  last_.info.triggerMs = status_.triggerMs;
  last_.info.window.startMs = endMs - SensorHistoryConfig::WINDOW_MS;
  last_.info.window.endMs = endMs;
  if (status_.queued == WindowSchedulerConfig::QUEUE_CAPACITY) {
    ++status_.dropped; // Preserve queued snapshots, especially the first window.
    return;
  }
  ScheduledWindow& slot = queue_[(head_ + status_.queued) % WindowSchedulerConfig::QUEUE_CAPACITY];
  last_.info.window = history.extract(endMs, slot.samples, WindowSchedulerConfig::SAMPLE_CAPACITY);
  slot.info = last_.info;
  ++status_.queued;
  last_.queued = true;
}

bool WindowScheduler::update(uint32_t now, const SensorHistory& history) {
  if (status_.state != WindowStreamState::RUNNING) return false;
  uint32_t lateness = now - status_.nextEndMs;
  if (lateness >= HALF_RANGE) return false;
  const uint32_t due = lateness / WindowSchedulerConfig::STEP_MS + 1;
  if (due > WindowSchedulerConfig::MAX_CATCHUP_WINDOWS) {
    const uint32_t skipped = due - WindowSchedulerConfig::MAX_CATCHUP_WINDOWS;
    status_.skipped += skipped;
    status_.nextEndMs += skipped * WindowSchedulerConfig::STEP_MS;
    status_.nextSequence += skipped;
    lateness = now - status_.nextEndMs;
  }
  const bool endpointAvailable = history.size() && history.newestMs() - status_.nextEndMs < HALF_RANGE;
  if (!endpointAvailable && lateness < WindowSchedulerConfig::END_WAIT_MS) return false;
  prepare(status_.nextEndMs, status_.nextSequence, history);
  status_.nextEndMs += WindowSchedulerConfig::STEP_MS; // Never drift to now/newest sample.
  ++status_.nextSequence;
  return true;
}

bool WindowScheduler::take(ScheduledWindow& destination) {
  if (!status_.queued) return false;
  const ScheduledWindow& slot = queue_[head_];
  if (slot.info.window.copied())
    memcpy(destination.samples, slot.samples, slot.info.window.count * sizeof(SensorSample));
  return discard(destination.info);
}

bool WindowScheduler::discard(ScheduledWindowInfo& destination) {
  if (!status_.queued) return false;
  destination = queue_[head_].info;
  head_ = (head_ + 1) % WindowSchedulerConfig::QUEUE_CAPACITY;
  --status_.queued;
  return true;
}
