#pragma once

#include "NodeTypes.h"
#include "SensorHistory.h"
#include "GenericPeakDetector.h"
#include "WindowScheduler.h"

// Shared implementation for the two independently uploadable ESP-IDF targets.
namespace ExerciseNode {
  void begin(NodeRole role);
  void update();
  // Call in the same main loop as update(); copy owns its data after return.
  SensorWindowResult copySensorWindow(uint32_t endMs, SensorSample* destination, size_t capacity);
  // Optional diagnostic observer; the scheduler receives fresh peak events directly.
  // One pending observation, preserving the first until consumed.
  bool consumeGenericPeak(GenericPeakEvent& event);
  // All window stream APIs run in the same main loop as update(). No transport calls.
  bool startWindowStream(uint32_t t0);
  void stopWindowStream();
  void armWindowStream();
  WindowStreamStatus windowStreamStatus();
  // Caller owns the copied snapshot. On ESP32, allocate destination statically.
  bool takeSensorWindow(ScheduledWindow& destination);
}
