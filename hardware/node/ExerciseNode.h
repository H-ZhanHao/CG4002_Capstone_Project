#pragma once

#include "NodeTypes.h"

// Shared implementation for the two independently uploadable ESP-IDF targets.
namespace ExerciseNode {
  void begin(NodeRole role);
  void update();
}
