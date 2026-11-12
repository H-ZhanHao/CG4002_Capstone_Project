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
const float rotating[] = {1.5f, 0, 0};

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

  void baseline(const float *direction = upright) { samples(30, direction); }

  void candidate() {
    samples(11, low);
    assert(!sample(impact)); // Impact alone no longer reports a fall.
  }

  void confirm(const float *direction = sideways) {
    const unsigned previous = events;
    samples(49, direction);
    assert(events == previous); // Wait the full 500 ms after impact.
    assert(sample(direction));
    assert(events == previous + 1);
  }
};

void confirmedFall(uint64_t start, const float *before, const float *after) {
  Trace trace(1, start);
  trace.baseline(before);
  trace.candidate();
  trace.confirm(after);
  trace.samples(10, after);
  assert(trace.events == 1); // Consumed exactly once.
}
}

int main() {
  confirmedFall(0, upright, sideways);
  confirmedFall(0, sideways, upright); // Relative posture, any mounting axis.
  confirmedFall((uint64_t(UINT32_MAX) - 600) * 1000, upright, sideways);

  // The existing feedback path starts only after confirmation and gives the
  // three-second fall alarm priority over rep sounds and session resets.
  Trace alarm;
  FeedbackController outputs;
  outputs.begin(1);
  alarm.baseline(); alarm.candidate();
  alarm.samples(49, sideways);
  assert(alarm.events == 0 && !outputs.buzzerOn());
  assert(alarm.sample(sideways));
  const uint32_t confirmed = static_cast<uint32_t>(alarm.now / 1000);
  assert(outputs.trigger(FeedbackController::Action::Rep, confirmed));
  assert(outputs.trigger(FeedbackController::Action::FallAlarm, confirmed));
  assert(outputs.trigger(FeedbackController::Action::Rep, confirmed + 50));
  outputs.resetSession();
  outputs.update(confirmed + FallConfig::BUZZER_DURATION_MS - 1);
  assert(outputs.buzzerOn());
  outputs.update(confirmed + FallConfig::BUZZER_DURATION_MS);
  assert(!outputs.buzzerOn());
  outputs.update(confirmed + FallConfig::BUZZER_DURATION_MS + 1000);
  assert(!outputs.buzzerOn()); // Suppressed reps never replay after the alarm.

  Trace jumps;
  jumps.baseline();
  for (int i = 0; i < 4; ++i) {
    jumps.candidate();
    jumps.samples(50, upright);
    assert(jumps.events == 0);
  }

  Trace moving;
  moving.baseline(); moving.candidate();
  moving.samples(50, sideways, rotating);
  assert(moving.events == 0); // Real gyro input must prevent settling.
  moving.baseline(); moving.candidate(); moving.confirm();

  Trace recovery;
  recovery.baseline(); recovery.candidate();
  recovery.samples(49, sideways); recovery.sample(upright);
  assert(recovery.events == 0);

  Trace expired;
  expired.baseline(); expired.samples(11, low);
  expired.samples(100, upright); expired.sample(impact);
  expired.samples(50, sideways);
  assert(expired.events == 0); // Hardware owns impact-window expiry.

  Trace startup;
  startup.candidate(); startup.samples(50, sideways);
  assert(startup.events == 0); // Requires a stable reference before freefall.

  // Match the hardware's 100 ms gap limit, including its inclusive boundary.
  for (uint64_t interval : {60000ULL, 100000ULL}) {
    Trace allowedGap;
    allowedGap.baseline(); allowedGap.candidate();
    allowedGap.sample(sideways, still, interval);
    while (allowedGap.now < 920000) allowedGap.sample(sideways);
    assert(allowedGap.events == 1);
  }

  // Missing data during confirmation must discard the candidate and recover.
  for (uint64_t interval : {100001ULL, 250000ULL,
                            (uint64_t(UINT32_MAX) + 1) * 1000 + 10000}) {
    Trace gap;
    gap.baseline(); gap.candidate(); gap.samples(25, sideways);
    gap.sample(sideways, still, interval); gap.samples(50, sideways);
    assert(gap.events == 0);
    gap.baseline(); gap.candidate(); gap.confirm();
  }

  for (float invalid : {NAN, INFINITY}) {
    for (int axis = 0; axis < 3; ++axis) {
      for (bool badGyro : {false, true}) {
        Trace bad;
        bad.baseline(); bad.candidate(); bad.samples(25, sideways);
        float broken[] = {0, 0, 0};
        broken[axis] = invalid;
        assert(!bad.sample(badGyro ? sideways : broken,
                           badGyro ? broken : still));
        bad.samples(50, sideways);
        assert(bad.events == 0);
        bad.baseline(); bad.candidate(); bad.confirm();
      }
    }
  }

  for (int missing = 0; missing < 3; ++missing) {
    Trace bad;
    bad.baseline(); bad.candidate(); bad.samples(25, sideways);
    assert(!bad.sample(missing == 0 ? nullptr : sideways,
                       missing == 1 ? nullptr : still, 10000, missing != 2));
    bad.samples(50, sideways);
    assert(bad.events == 0);
    bad.baseline(); bad.candidate(); bad.confirm();
  }

  for (bool backwards : {false, true}) {
    Trace clock;
    clock.baseline(); clock.candidate(); clock.samples(25, sideways);
    if (backwards) clock.now -= 10000;
    assert(!clock.sample(sideways, still, 0));
    clock.samples(50, sideways);
    assert(clock.events == 0);
    clock.baseline(); clock.candidate(); clock.confirm();
  }

  Trace cooldown;
  cooldown.baseline(); cooldown.candidate(); cooldown.confirm();
  const uint64_t firstAlarm = cooldown.now;
  cooldown.baseline(); cooldown.candidate(); cooldown.samples(50, sideways);
  assert(cooldown.events == 1);
  while (cooldown.now < firstAlarm + FallConfig::BUZZER_DURATION_MS * 1000ULL)
    cooldown.sample(upright);
  cooldown.baseline(); cooldown.candidate(); cooldown.confirm();
  assert(cooldown.events == 2);

  Trace wrist(2);
  wrist.baseline(); wrist.candidate(); wrist.samples(50, sideways);
  assert(wrist.events == 0);
  confirmedFall(0, upright, sideways); // Reinitialization clears role/cooldown.

  puts("Fall bridge: confirmation, gyro, posture, gaps, invalid inputs, clock rollover, cooldown, Hip-only and alarm priority checks passed");
}
