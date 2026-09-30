#include <ExerciseNode.h>

// Upload only to the hip board. Settings are shared in node/ExerciseConfig.h.
void setup() {
  ExerciseNode::begin(HIP);
}

void loop() {
  ExerciseNode::update();
}
