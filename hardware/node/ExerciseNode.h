#pragma once

#include "NodeTypes.h"

// Shared implementation for the two independently uploadable sketches.
namespace ExerciseNode {
  void begin(NodeRole role);
  void update();
}
