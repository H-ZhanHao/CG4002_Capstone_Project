#include "fall_bridge.h"
#include "FeedbackController.h"
#include <assert.h>
#include <stdio.h>

namespace {
const float upright[] = {0, 0, 9.81f};
const float sideways[] = {9.81f, 0, 0};
const float low[] = {0, 0, 1};
const float impact[] = {0, 0, 30};
const float still[] = {0, 0, 0};
uint64_t now;

bool sample(const float acceleration[3], const float gyro[3] = still) {
  now += 10000;
  return fall_bridge_update(acceleration, gyro, now, true);
}

void baseline(uint8_t node) {
  fall_bridge_begin(node);
  now = 0;
  for (int i = 0; i < 30; ++i) assert(!sample(upright));
}

void candidate() {
  for (int i = 0; i < 11; ++i) assert(!sample(low));
  assert(!sample(impact)); // Impact alone must not start the alarm.
}
}

int main() {
  baseline(1);
  FeedbackController outputs;
  outputs.begin(1);
  candidate();
  for (int i = 0; i < 49; ++i) assert(!sample(sideways));
  assert(!outputs.buzzerOn());
  assert(sample(sideways)); // Confirmed 500 ms after impact.
  const uint32_t confirmed = static_cast<uint32_t>(now / 1000);
  assert(outputs.trigger(FeedbackController::Action::Rep, confirmed));
  assert(outputs.trigger(FeedbackController::Action::FallAlarm, confirmed));
  assert(outputs.trigger(FeedbackController::Action::Rep, confirmed + 50));
  assert(!sample(sideways)); // Event consumed once.
  outputs.update(confirmed + 2999);
  assert(outputs.buzzerOn());
  outputs.update(confirmed + 3000);
  assert(!outputs.buzzerOn());
  outputs.update(confirmed + 4000);
  assert(!outputs.buzzerOn()); // Suppressed rep never replays after the fall.

  baseline(1);
  candidate();
  for (int i = 0; i < 50; ++i) assert(!sample(upright)); // Jump-like recovery.

  baseline(1);
  candidate();
  const float rotating[] = {1.5f, 0, 0};
  for (int i = 0; i < 50; ++i) assert(!sample(sideways, rotating));

  baseline(2);
  candidate();
  for (int i = 0; i < 50; ++i) assert(!sample(sideways)); // Wrist never alarms.

  puts("Fall adapter: confirmed Hip falls, gyro, recovery and alarm priority PASS");
}
