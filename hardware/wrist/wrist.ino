#include <ExerciseNode.h>

// Upload only to the wrist board. Settings are shared in node/ExerciseConfig.h.
void setup() {
  ExerciseNode::begin(WRIST);
}

void loop() {
  ExerciseNode::update();
}
