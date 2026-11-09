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
const float rotating[] = {1.5f, 0, 0};

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
    samples(11, low); // 100 ms from first low sample.
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
    moving.samples(50, useGyro ? sideways : accelerating, useGyro ? rotating : still);
    assert(!moving.detector.consumeFallEvent());
  }

  // Transient tilt and filter lag cannot confirm a return to upright.
  Trace recovery;
  recovery.baseline(); recovery.candidate();
  recovery.samples(49, sideways);
  recovery.sample(upright);
  assert(!recovery.detector.consumeFallEvent());

  // Too little settled evidence at the deadline rejects the candidate.
  Trace late;
  late.baseline(); late.candidate();
  late.samples(31, sideways, rotating);
  late.samples(19, sideways);
  assert(!late.detector.consumeFallEvent());

  // Exactly 200 ms of settled evidence is sufficient at 500 ms.
  Trace settling;
  settling.baseline(); settling.candidate();
  settling.samples(29, sideways, rotating);
  settling.samples(21, sideways);
  assert(settling.detector.consumeFallEvent());

  // A small lean is not a fall posture, even when motion stops.
  Trace lean;
  lean.baseline(); lean.candidate();
  const float leaning[] = {4.905f, 0, 8.4957f}; // 30 degrees.
  lean.samples(50, leaning);
  assert(!lean.detector.consumeFallEvent());

  Trace shortFlight;
  shortFlight.baseline();
  shortFlight.samples(10, low); // Only 90 ms.
  shortFlight.sample(impact);
  shortFlight.samples(50, sideways);
  assert(!shortFlight.detector.consumeFallEvent());

  Trace softImpact;
  softImpact.baseline(); softImpact.samples(11, low);
  const float soft[] = {0, 0, 25}; // Threshold is strictly greater than 25.
  softImpact.sample(soft);
  softImpact.samples(110, sideways);
  assert(!softImpact.detector.consumeFallEvent());

  Trace expired;
  expired.baseline(); expired.samples(11, low);
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
