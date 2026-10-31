#include <assert.h>
#include <stdio.h>
#include <vector>

#include "GenericPeakDetector.h"

struct Rig {
  GenericPeakDetector detector;
  std::vector<GenericPeakEvent> events;
  uint32_t timeMs = 0;

  void feed(float accelerationMotion, float rotation, uint32_t elapsedMs = 10) {
    timeMs += elapsedMs;
    SensorSample sample = {};
    sample.timestampMs = timeMs;
    sample.acceleration[2] = GeneralConfig::GRAVITY_MPS2 + accelerationMotion;
    sample.gyro[0] = rotation;
    GenericPeakEvent event;
    if (detector.update(sample, event)) events.push_back(event);
  }

  void hold(int samples, float accelerationMotion = 0, float rotation = 0) {
    for (int i = 0; i < samples; ++i) feed(accelerationMotion, rotation);
  }
};

int main() {
  static_assert(GenericPeakConfig::MIN_ACTIVE_MS == 100,
                "generic peaks must reject motion shorter than 100 ms");

  for (bool gyroOnly : {false, true}) {
    Rig brief;
    brief.hold(20);
    brief.hold(8, gyroOnly ? 0 : 1.2f, gyroOnly ? 1.2f : 0);
    brief.hold(20);
    assert(brief.events.empty());

    Rig sustained;
    sustained.hold(20);
    sustained.hold(20, gyroOnly ? 0 : .70f, gyroOnly ? .70f : 0);
    sustained.hold(20);
    assert(sustained.events.size() == 1);
  }

  puts("generic peak 100 Hz sensitivity tests: PASS");
  return 0;
}
