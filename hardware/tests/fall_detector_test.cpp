#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "FallDetector.h"

namespace {
struct Trace {
  FallDetector detector;
  unsigned long now;

  explicit Trace(unsigned long start = 0) : now(start) { detector.begin(now); }

  void sample(float magnitude, unsigned long interval = 10) {
    now += interval;
    detector.update(magnitude, now);
  }

  void samples(unsigned count, float magnitude) {
    for (unsigned i = 0; i < count; ++i) sample(magnitude);
  }

  void freefall() {
    samples(11, 1.0f); // 100 ms since the first low sample.
    assert(detector.state() == WAITING_FOR_IMPACT);
    assert(!detector.consumeFallEvent());
  }
};

void detectsFall(unsigned long start) {
  Trace trace(start);
  trace.freefall(); // No initial gravity baseline is needed.
  trace.sample(30.0f);
  assert(trace.detector.consumeFallEvent()); // Immediate; no posture/gyro delay.
  assert(!trace.detector.consumeFallEvent());
  assert(trace.detector.state() == MONITORING_NORMAL);
}
}

int main() {
  detectsFall(0);
  detectsFall(ULONG_MAX - 60); // Native unsigned-long timer rollover.

  Trace impactOnly;
  impactOnly.sample(30.0f);
  assert(!impactOnly.detector.consumeFallEvent());

  Trace mildUnloading;
  mildUnloading.samples(20, 6.0f);
  mildUnloading.sample(30.0f);
  assert(!mildUnloading.detector.consumeFallEvent());

  Trace threshold;
  threshold.samples(20, 4.0f); // Freefall comparison is strictly below 4.
  assert(threshold.detector.state() == MONITORING_NORMAL);
  threshold.sample(3.99f);
  threshold.sample(3.99f, 99);
  assert(threshold.detector.state() == POSSIBLE_FREE_FALL);
  threshold.sample(3.99f, 1);
  assert(threshold.detector.state() == WAITING_FOR_IMPACT);
  threshold.sample(25.0f); // Impact comparison is strictly above 25.
  assert(!threshold.detector.consumeFallEvent());
  threshold.sample(25.01f);
  assert(threshold.detector.consumeFallEvent());

  Trace shortFlight;
  shortFlight.samples(10, 1.0f); // Only 90 ms since the first low sample.
  shortFlight.sample(30.0f);
  assert(!shortFlight.detector.consumeFallEvent());
  assert(shortFlight.detector.state() == MONITORING_NORMAL);

  Trace interrupted;
  interrupted.samples(6, 1.0f);
  interrupted.sample(4.0f); // Returning to the threshold cancels freefall.
  interrupted.samples(6, 1.0f);
  interrupted.sample(30.0f);
  assert(!interrupted.detector.consumeFallEvent());

  Trace softLanding;
  softLanding.freefall();
  softLanding.sample(17.0f);
  softLanding.samples(101, 9.81f);
  assert(!softLanding.detector.consumeFallEvent());
  assert(softLanding.detector.state() == MONITORING_NORMAL);

  Trace boundary;
  boundary.freefall();
  boundary.sample(30.0f, 1000);
  assert(boundary.detector.consumeFallEvent());

  Trace expired;
  expired.freefall();
  expired.sample(9.81f, 1001); // A non-impact reading expires the wait.
  assert(expired.detector.state() == MONITORING_NORMAL);
  expired.sample(30.0f);
  assert(!expired.detector.consumeFallEvent());

  // Preserve cfcb6ac exactly: on a single update, impact is checked before
  // expiry. Do not silently change the historical state-machine ordering.
  Trace impactFirst;
  impactFirst.freefall();
  impactFirst.sample(30.0f, 1001);
  assert(impactFirst.detector.consumeFallEvent());

  Trace reset;
  reset.freefall();
  reset.detector.begin(reset.now);
  reset.sample(30.0f);
  assert(!reset.detector.consumeFallEvent());
  reset.freefall(); reset.sample(30.0f);
  reset.detector.begin(reset.now); // Also clears an unconsumed event.
  assert(!reset.detector.consumeFallEvent());

  puts("Legacy fall detector: freefall/impact thresholds, duration, reset, expiry ordering, immediate events and timer rollover passed");
}
