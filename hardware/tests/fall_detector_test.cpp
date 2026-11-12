#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <initializer_list>

#include "ExerciseConfig.h"
#include "FallDetector.h"

namespace {
const float upright[] = {0, 0, 9.81f};
const float sideways[] = {9.81f, 0, 0};
const float low[] = {0, 0, 1};
const float impact[] = {0, 0, 30};
const float still[] = {0, 0, 0};
const float rotating[] = {3.0f, 0, 0};

struct Trace {
  FallDetector detector;
  uint32_t now;

  explicit Trace(uint32_t start = 0) : now(start) { detector.begin(now); }

  void sample(const float acceleration[3], const float gyro[3] = still,
              uint32_t interval = 10) {
    now += interval;
    detector.update(acceleration, gyro, now);
  }

  void samples(int count, const float acceleration[3], const float gyro[3] = still) {
    for (int i = 0; i < count; ++i) sample(acceleration, gyro);
  }

  void baseline(const float direction[3] = upright) { samples(30, direction); }

  void candidate() {
    samples(5, low); // 40 ms from first low sample.
    assert(detector.state() == WAITING_FOR_IMPACT);
    sample(impact);
    assert(detector.state() == CONFIRMING_FALL);
    assert(!detector.consumeFallEvent());
  }
};

void confirmFall(uint32_t start, const float before[3], const float after[3]) {
  Trace trace(start);
  trace.baseline(before);
  trace.candidate();
  for (int i = 0; i < 49; ++i) {
    trace.sample(after);
    assert(!trace.detector.consumeFallEvent());
  }
  trace.sample(after);
  assert(trace.detector.consumeFallEvent());
  assert(!trace.detector.consumeFallEvent()); // Event delivered once.
  assert(trace.detector.state() == MONITORING_NORMAL);
}
}

int main() {
  confirmFall(0, upright, sideways);
  confirmFall(UINT32_MAX - 600, upright, sideways); // Timer rollover during confirmation.
  confirmFall(0, sideways, upright); // No hardcoded sensor mounting axis.

  // Repeated jump-like flight/impact followed by upright rest never alerts.
  Trace jumps;
  jumps.baseline();
  for (int jump = 0; jump < 8; ++jump) {
    jumps.candidate();
    for (int i = 0; i < 50; ++i) {
      jumps.sample(upright);
      assert(!jumps.detector.consumeFallEvent());
    }
  }

  // Continued rotation or linear acceleration must not count as settling.
  for (bool useGyro : {false, true}) {
    Trace moving;
    moving.baseline(); moving.candidate();
    const float accelerating[] = {13.0f, 0, 0};
    moving.samples(150, useGyro ? sideways : accelerating, useGyro ? rotating : still);
    assert(!moving.detector.consumeFallEvent());
    assert(moving.detector.state() == MONITORING_NORMAL);
  }

  // Transient tilt and filter lag cannot confirm a return to upright.
  Trace recovery;
  recovery.baseline(); recovery.candidate();
  recovery.samples(49, sideways);
  recovery.sample(upright);
  assert(!recovery.detector.consumeFallEvent());

  // Movement at 500 ms gets extra time to settle instead of losing the fall.
  Trace late;
  late.baseline(); late.candidate();
  late.samples(70, sideways, rotating);
  late.samples(10, sideways);
  assert(!late.detector.consumeFallEvent());
  late.sample(sideways);
  assert(late.detector.consumeFallEvent());

  // Exactly 100 ms of settled evidence is sufficient at 500 ms.
  Trace settling;
  settling.baseline(); settling.candidate();
  settling.samples(39, sideways, rotating);
  settling.samples(11, sideways);
  assert(settling.detector.consumeFallEvent());

  // A small lean is not a fall posture, even when motion stops.
  Trace lean;
  lean.baseline(); lean.candidate();
  const float leaning[] = {4.905f, 0, 8.4957f}; // 30 degrees.
  lean.samples(50, leaning);
  assert(!lean.detector.consumeFallEvent());

  Trace shortFlight;
  shortFlight.baseline();
  shortFlight.samples(2, low); // Only 10 ms; impact can independently qualify.
  shortFlight.sample(impact);
  shortFlight.samples(50, sideways);
  assert(shortFlight.detector.consumeFallEvent());

  Trace softImpact;
  softImpact.baseline();
  const float soft[] = {0, 0, 17}; // No freefall, much softer than the old 25 gate.
  softImpact.sample(soft);
  const float tilted[] = {6.3057f, 0, 7.5149f}; // 40 degrees, below old posture gate.
  const float residualMotion[] = {1.2f, 0, 0};
  softImpact.samples(50, tilted, residualMotion);
  assert(softImpact.detector.consumeFallEvent());

  // A soft collapse can qualify through unloading and posture, without impact.
  Trace collapse;
  collapse.baseline();
  const float unloaded[] = {0, 0, 6};
  collapse.samples(5, unloaded);
  collapse.samples(51, sideways);
  assert(collapse.detector.consumeFallEvent());

  // Short unloading without impact, ordinary posture changes, and subthreshold
  // bumps alone must not become fall candidates.
  Trace ordinary;
  ordinary.baseline(); ordinary.samples(4, unloaded);
  ordinary.samples(160, sideways);
  assert(!ordinary.detector.consumeFallEvent());
  ordinary.baseline();
  const float belowImpact[] = {16, 0, 0};
  ordinary.sample(belowImpact); ordinary.samples(160, upright);
  assert(!ordinary.detector.consumeFallEvent());

  // Repeated shorter jump flights and soft landings stay upright and are rejected.
  Trace quickJumps;
  quickJumps.baseline();
  for (int jump = 0; jump < 12; ++jump) {
    quickJumps.samples(2, unloaded);
    quickJumps.sample(soft);
    quickJumps.samples(25, upright);
    assert(!quickJumps.detector.consumeFallEvent());
  }
  quickJumps.samples(160, upright);
  assert(!quickJumps.detector.consumeFallEvent());
  // A fall during jumping is still eligible; exercise never disables detection.
  quickJumps.sample(soft); quickJumps.samples(50, sideways);
  assert(quickJumps.detector.consumeFallEvent());

  Trace expired;
  expired.baseline(); expired.samples(5, low);
  expired.samples(100, upright); // Still no impact at the inclusive deadline.
  expired.sample(impact); // Impact now lies outside the permitted window.
  assert(expired.detector.state() == MONITORING_NORMAL);
  expired.samples(50, sideways);
  assert(!expired.detector.consumeFallEvent());

  // No inferred posture without first collecting a stable reference.
  Trace startup;
  startup.samples(11, low); startup.sample(impact); startup.samples(50, sideways);
  assert(!startup.detector.consumeFallEvent());

  Trace gap;
  gap.baseline(); gap.candidate(); gap.samples(25, sideways);
  gap.sample(sideways, still, 250);
  assert(!gap.detector.consumeFallEvent());
  assert(gap.detector.state() == MONITORING_NORMAL);
  gap.baseline(); gap.candidate(); gap.samples(50, sideways);
  assert(gap.detector.consumeFallEvent()); // Recovers after new baseline.

  Trace freefallGap;
  freefallGap.baseline(); freefallGap.sample(low);
  freefallGap.sample(low, still, 120); freefallGap.sample(impact);
  assert(freefallGap.detector.state() == MONITORING_NORMAL);

  for (float invalid : {NAN, INFINITY}) {
    Trace bad;
    bad.baseline(); bad.candidate(); bad.samples(25, sideways);
    const float broken[] = {invalid, 0, 0};
    bad.sample(broken); bad.samples(25, sideways);
    assert(!bad.detector.consumeFallEvent());
    bad.baseline(); bad.candidate();
    bad.sample(sideways, broken); bad.samples(50, sideways);
    assert(!bad.detector.consumeFallEvent());
  }

  puts("Fall confirmation: timing, posture, jumps, movement, recovery, thresholds, gaps, invalid data and rollover passed");
}
