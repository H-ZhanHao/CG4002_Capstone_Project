#pragma once

#include "SensorHistory.h"

namespace WindowSchedulerConfig {
  constexpr uint32_t STEP_MS = 1000;
  constexpr uint32_t END_WAIT_MS = 100; // Allow the next sensor read to cover an endpoint.
  // Five seconds including both endpoints, plus one sample for boundary timing jitter.
  constexpr size_t SAMPLE_CAPACITY = SensorHistoryConfig::WINDOW_MS * 1000 /
    GeneralConfig::SAMPLE_INTERVAL_US + 2;
  constexpr size_t QUEUE_CAPACITY = 3;
  constexpr uint32_t MAX_CATCHUP_WINDOWS = 3;
  static_assert(STEP_MS > 0 && END_WAIT_MS < STEP_MS, "Invalid window cadence");
  static_assert(QUEUE_CAPACITY > 0 && MAX_CATCHUP_WINDOWS > 0, "Use bounded nonempty queues");
}

struct ScheduledWindowInfo {
  uint32_t session = 0, sequence = 0, triggerMs = 0;
  SensorWindowResult window;
};

struct ScheduledWindow {
  ScheduledWindowInfo info;
  // Only [0, info.window.count) is valid, and only when window.copied() is true.
  SensorSample samples[WindowSchedulerConfig::SAMPLE_CAPACITY] = {};
};

enum class WindowStreamState { ARMED, RUNNING, STOPPED };

struct WindowStreamStatus {
  WindowStreamState state = WindowStreamState::ARMED;
  uint32_t session = 0, triggerMs = 0, nextEndMs = 0, nextSequence = 0;
  size_t queued = 0;
  uint32_t dropped = 0, skipped = 0;
};

struct WindowPreparation {
  ScheduledWindowInfo info;
  // False means QUEUE_FULL: no extraction was attempted; ignore window.status.
  bool queued = false;
};

// Single main-loop owner; no transport, waiting, allocation or exercise control.
// Modular time comparisons require timestamps less than 2^31 ms apart.
class WindowScheduler {
 public:
  void reset(); // Boot: clear all state, arm automatic triggering.
  void arm(); // Arm only when not running; an active session keeps its anchor.
  void stop(); // Disable automatic triggering and discard queued windows.
  // Explicit start, including from STOPPED. Reject future t0 / active-session restarts.
  // Captures sequence zero immediately, including an error record if unavailable.
  bool start(uint32_t t0, uint32_t now, const SensorHistory& history);
  // At most one window attempt per call. True means lastPreparation() changed.
  bool update(uint32_t now, const SensorHistory& history);
  // Copy before removing; the caller owns the result. False leaves it unchanged.
  // Use static/heap storage on ESP32: a full snapshot is larger than the task stack.
  bool take(ScheduledWindow& destination);
  WindowStreamStatus status() const { return status_; }
  const WindowPreparation& lastPreparation() const { return last_; }

 private:
  void prepare(uint32_t endMs, uint32_t sequence, const SensorHistory& history);
  ScheduledWindow queue_[WindowSchedulerConfig::QUEUE_CAPACITY];
  size_t head_ = 0;
  WindowStreamStatus status_;
  WindowPreparation last_;
};
