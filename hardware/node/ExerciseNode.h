#pragma once

#include "NodeTypes.h"
#include "SensorHistory.h"

// Shared implementation for the two independently uploadable ESP-IDF targets.
namespace ExerciseNode {
  void begin(NodeRole role);
  void update();
  // Call in the same main loop as update(); copy owns its data after return.
  SensorWindowResult copySensorWindow(uint32_t endMs, SensorSample* destination, size_t capacity);
}
