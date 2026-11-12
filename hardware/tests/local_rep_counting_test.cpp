#include <assert.h>
#include <stdio.h>
#include "LocalRepCounting.h"

int main() {
  LocalRepCounting local;
  unsigned long now = 0;
  unsigned int events = 0;
  auto samples = [&](int count, float acceleration, float gyroX) {
    const float gyro[3] = {gyroX, 0, 0};
    for (int i = 0; i < count; ++i) {
      now += 20;
      if (local.update(acceleration, gyro, now)) ++events;
    }
  };
  assert(local.begin(SQUAT, PAUSE_MODE, now));
  samples(10, 9.81f, 0);
  samples(25, 11.4f, 0);
  samples(45, 9.81f, 0);
  assert(local.count() == 1 && events == 1);
  assert(local.begin(JUMPING_JACK, CONTINUOUS_MODE, now));
  assert(local.count() == 0 && local.axis() == -1);
  samples(65, 9.81f, 3.0f);
  samples(35, 9.81f, -3.0f);
  samples(25, 9.81f, 0);
  assert(local.count() == 1 && events == 2 && local.axis() == 0);
  puts("Extracted local counting retains pause/continuous detection and reset behavior");
}
