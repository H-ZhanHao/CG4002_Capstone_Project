#include "fall_bridge.h"
#include "../../../../../../hardware/node/FallDetector.h"
#include "../../../../../../hardware/node/ExerciseConfig.h"
#include <math.h>

namespace {
FallDetector detector;
bool hip, haveSample, haveAlarm;
uint64_t lastSample, lastAlarm;
constexpr uint64_t MAX_GAP_US = FallConfig::MAX_SAMPLE_GAP_MS * 1000ULL;
void reset(uint64_t now) {
  detector.begin(static_cast<unsigned long>(now / 1000));
}
}
extern "C" void fall_bridge_begin(uint8_t node) {
  hip = node == 1;
  haveSample = haveAlarm = false;
  reset(0);
}
extern "C" bool fall_bridge_update(const float a[3], const float gyro[3],
                                   uint64_t now, bool valid) {
  if (!hip) return false;
  valid = valid && a && gyro &&
      isfinite(a[0]) && isfinite(a[1]) && isfinite(a[2]) &&
      isfinite(gyro[0]) && isfinite(gyro[1]) && isfinite(gyro[2]);
  if (!valid) { reset(now); haveSample = false; return false; }
  if (haveSample && (now <= lastSample || now - lastSample > MAX_GAP_US)) reset(now);
  haveSample = true;
  lastSample = now;
  if (haveAlarm && now >= lastAlarm &&
      now - lastAlarm < FallConfig::BUZZER_DURATION_MS * 1000ULL) {
    reset(now);
    return false;
  }
  // Hardware owns freefall, impact expiry, posture and settling confirmation.
  detector.update(a, gyro, static_cast<unsigned long>(now / 1000));
  if (!detector.consumeFallEvent()) return false;
  haveAlarm = true;
  lastAlarm = now;
  return true;
}
