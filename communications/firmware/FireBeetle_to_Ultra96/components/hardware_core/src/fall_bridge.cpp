#include "fall_bridge.h"
#include "../../../../../../hardware/node/FallDetector.h"
#include "../../../../../../hardware/node/ExerciseConfig.h"
#include <math.h>

namespace {
FallDetector detector;
bool hip, haveSample, waiting, haveAlarm;
uint64_t lastSample, impactStart, lastAlarm;
constexpr uint64_t MAX_GAP_US = 50000;
void reset(uint64_t now) {
  detector.begin(static_cast<unsigned long>(now / 1000));
  waiting = false;
}
}
extern "C" void fall_bridge_begin(uint8_t node) {
  hip = node == 1;
  haveSample = haveAlarm = false;
  reset(0);
}
extern "C" bool fall_bridge_update(const float a[3], uint64_t now, bool valid) {
  if (!hip) return false;
  valid = valid && a && isfinite(a[0]) && isfinite(a[1]) && isfinite(a[2]);
  if (!valid) { reset(now); haveSample = false; return false; }
  if (haveSample && (now <= lastSample || now - lastSample > MAX_GAP_US)) reset(now);
  haveSample = true;
  lastSample = now;
  if (haveAlarm && now >= lastAlarm &&
      now - lastAlarm < FallConfig::BUZZER_DURATION_MS * 1000ULL) {
    reset(now);
    return false;
  }
  // Canonical detector checks impact before expiry. Guard an expired window
  // here so a late impact cannot complete an unrelated sequence.
  if (waiting && now - impactStart > FallConfig::IMPACT_WINDOW_MS * 1000ULL) reset(now);
  const float magnitude = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
  if (!isfinite(magnitude)) { reset(now); return false; }
  detector.update(magnitude, static_cast<unsigned long>(now / 1000));
  if (!waiting && detector.state() == WAITING_FOR_IMPACT) {
    impactStart = now;
    waiting = true;
  }
  if (detector.state() != WAITING_FOR_IMPACT) waiting = false;
  if (!detector.consumeFallEvent()) return false;
  haveAlarm = true;
  lastAlarm = now;
  return true;
}
