#include "fall_bridge.h"
#include "ExerciseConfig.h"
#include "FeedbackController.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <initializer_list>

namespace {
const float upright[] = {0, 0, 9.81f};
const float sideways[] = {9.81f, 0, 0};
const float low[] = {0, 0, 1};
const float impact[] = {0, 0, 30};
const float still[] = {0, 0, 0};
const float rotating[] = {3.0f, 0, 0};

struct Trace {
  uint64_t now;
  unsigned events = 0;

  explicit Trace(uint8_t node = 1, uint64_t start = 0) : now(start) {
    fall_bridge_begin(node);
  }

  bool sample(const float *a, const float *g = still,
              uint64_t interval = 10000, bool valid = true) {
    now += interval;
    const bool event = fall_bridge_update(a, g, now, valid);
    if (event) ++events;
    return event;
  }

  void samples(unsigned count, const float *a, const float *g = still) {
    for (unsigned i = 0; i < count; ++i) sample(a, g);
  }

  void freefall() { samples(11, low); } // 100 ms of sampled freefall.
};
}

int main() {
  // Same old algorithm at startup and across the ESP's 32-bit millisecond wrap.
  for (uint64_t start : {0ULL, (uint64_t(UINT32_MAX) - 60) * 1000}) {
    Trace trace(1, start);
    trace.freefall();
    assert(trace.events == 0);
    assert(trace.sample(impact)); // Immediate, without baseline or posture.
    trace.samples(10, sideways);
    assert(trace.events == 1);
  }

  // Magnitude uses every axis; finite gyro motion does not gate the old logic.
  Trace axes;
  axes.freefall();
  const float diagonalImpact[] = {18, 18, 0};
  assert(axes.sample(diagonalImpact, rotating));

  Trace ordinary;
  ordinary.sample(impact); // Impact without freefall is insufficient.
  const float unloaded[] = {0, 0, 6};
  ordinary.samples(20, unloaded); ordinary.sample(impact);
  ordinary.samples(10, low); ordinary.sample(impact); // Only 90 ms low.
  ordinary.freefall();
  const float soft[] = {0, 0, 17};
  ordinary.sample(soft); ordinary.samples(101, sideways);
  assert(ordinary.events == 0); // Soft collapse/posture alone cannot alert.

  // Maintain the bridge's inclusive 100 ms gap limit and reject larger gaps.
  for (uint64_t interval : {60000ULL, 100000ULL}) {
    Trace allowed;
    allowed.freefall(); allowed.sample(upright, still, interval);
    assert(allowed.sample(impact));
  }
  for (uint64_t interval : {100001ULL, 250000ULL,
                            (uint64_t(UINT32_MAX) + 1) * 1000 + 10000}) {
    Trace gap;
    gap.freefall(); gap.sample(upright, still, interval);
    assert(!gap.sample(impact));
    gap.freefall(); assert(gap.sample(impact)); // Recovers with fresh evidence.
  }

  // Invalid acceleration OR gyro still cancels candidates in the wrapper.
  for (float invalid : {NAN, INFINITY}) {
    for (int axis = 0; axis < 3; ++axis) {
      for (bool badGyro : {false, true}) {
        Trace bad;
        bad.freefall();
        float broken[] = {0, 0, 0};
        broken[axis] = invalid;
        assert(!bad.sample(badGyro ? upright : broken,
                           badGyro ? broken : still));
        assert(!bad.sample(impact));
        bad.freefall(); assert(bad.sample(impact));
      }
    }
  }
  for (int missing = 0; missing < 3; ++missing) {
    Trace bad;
    bad.freefall();
    assert(!bad.sample(missing == 0 ? nullptr : upright,
                       missing == 1 ? nullptr : still, 10000, missing != 2));
    assert(!bad.sample(impact));
    bad.freefall(); assert(bad.sample(impact));
  }
  for (bool backwards : {false, true}) {
    Trace clock;
    clock.freefall();
    if (backwards) clock.now -= 10000;
    assert(!clock.sample(upright, still, 0));
    assert(!clock.sample(impact));
    clock.freefall(); assert(clock.sample(impact));
  }

  Trace cooldown;
  cooldown.freefall(); assert(cooldown.sample(impact));
  const uint64_t firstAlarm = cooldown.now;
  cooldown.freefall(); assert(!cooldown.sample(impact));
  assert(cooldown.events == 1);
  while (cooldown.now < firstAlarm + FallConfig::BUZZER_DURATION_MS * 1000ULL)
    cooldown.sample(upright);
  cooldown.freefall(); assert(cooldown.sample(impact));
  assert(cooldown.events == 2);

  Trace wrist(2);
  wrist.freefall(); assert(!wrist.sample(impact));
  assert(wrist.events == 0);

  // Runtime OFF/ON cancels partial evidence while preserving alarm cooldown.
  Trace switched;
  switched.freefall(); fall_bridge_reset(switched.now);
  assert(!switched.sample(impact));
  switched.freefall(); assert(switched.sample(impact));
  fall_bridge_reset(switched.now);
  switched.freefall(); assert(!switched.sample(impact));
  assert(switched.events == 1);

  // A genuine event still drives the three-second alarm, ahead of rep beeps.
  Trace alarm;
  FeedbackController outputs;
  outputs.begin(1);
  alarm.freefall();
  assert(!outputs.buzzerOn() && alarm.events == 0);
  assert(alarm.sample(impact));
  const uint32_t detected = static_cast<uint32_t>(alarm.now / 1000);
  assert(outputs.trigger(FeedbackController::Action::Rep, detected));
  assert(outputs.trigger(FeedbackController::Action::FallAlarm, detected));
  assert(outputs.trigger(FeedbackController::Action::Rep, detected + 50));
  outputs.resetSession();
  outputs.update(detected + FallConfig::BUZZER_DURATION_MS - 1);
  assert(outputs.buzzerOn());
  outputs.update(detected + FallConfig::BUZZER_DURATION_MS);
  assert(!outputs.buzzerOn());
  outputs.update(detected + FallConfig::BUZZER_DURATION_MS + 1000);
  assert(!outputs.buzzerOn());

  puts("Legacy fall bridge: magnitude, immediate events, gaps, invalid inputs, clock rollover, cooldown, runtime reset, Hip-only and alarm priority passed");
}
