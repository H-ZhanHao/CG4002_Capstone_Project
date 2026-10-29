#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ExercisePresets.h"
#include "RepCounter.h"
#include "ContinuousCounter.h"

static void movement(RepCounter& counter, unsigned long& now, float gravity,
                     float peak, int samples = 25) {
  for (int i = 0; i < samples; ++i) counter.update(gravity + peak, now += 20);
  for (int i = 0; i < 30; ++i) counter.update(gravity, now += 20);
}

int main() {
  const NodeRole roles[] = {HIP, WRIST, HIP, HIP, HIP, WRIST};
  unsigned long now = 0;
  for (int i = 0; i < EXERCISE_COUNT; ++i) {
    const ExerciseType exercise = (ExerciseType)i;
    assert(ExercisePresets::defaultMode(exercise) ==
           (exercise == JUMPING_JACK ? CONTINUOUS_MODE : PAUSE_MODE));
    assert(ExercisePresets::allowed(roles[i], exercise));
    assert(!ExercisePresets::allowed(roles[i] == HIP ? WRIST : HIP, exercise));
    RepCounter counter(getGenericSettings());
    const PauseCalibration preset = ExercisePresets::pause(exercise);
    // Activation/reset load fixed limits without collecting baseline samples.
    for (int session = 0; session < 2; ++session) {
      const float gravity = GeneralConfig::GRAVITY_MPS2;
      const unsigned long startedAt = now;
      assert(counter.beginPreset(now, preset));
      assert(counter.baselineMagnitude() == gravity);
      assert(counter.state() == REP_COUNTING);
      assert(counter.repetitionCount() == 0 && counter.calibrationCount() == 0);
      assert(!counter.consumeRepEvent() && !counter.consumeCalibrationEvent());
      // Start moving immediately, including at uptime zero; no quiet warm-up.
      movement(counter, now, gravity, preset.requiredPeak + 0.1f);
      assert(counter.repetitionCount() == 1); // First rep, no calibration pair.
      assert(now - startedAt < GeneralConfig::BASELINE_TIME_MS);
      assert(counter.consumeRepEvent());
      for (int n = 0; n < 100; ++n) counter.update(gravity, now += 20);
      assert(counter.repetitionCount() == 1);
      movement(counter, now, gravity, (preset.highThreshold + preset.requiredPeak) / 2);
      assert(counter.repetitionCount() == 1);
      assert(counter.consumeRepResult() == REP_TOO_WEAK);
      movement(counter, now, gravity, preset.requiredPeak + 0.1f, 5);
      assert(counter.repetitionCount() == 1);
      assert(counter.consumeRepResult() == REP_TOO_SHORT);
    }

    ContinuousCounter continuous;
    // Each new session discovers the axis and either starting direction anew.
    for (int session = 0; session < 2; ++session) {
      const float* bias = GeneralConfig::GYRO_BIAS_RAD_S;
      const int axis = (i + session) % 3;
      const float sign = session ? -1.0f : 1.0f;
      continuous.beginPreset(bias, now, ExercisePresets::continuous(exercise));
      assert(continuous.calibrated());
      assert(continuous.axis() == -1 && continuous.calibrationCount() == 0);
      assert(continuous.repetitionCount() == 0 && !continuous.consumeRepEvent());
      // Discover the axis from immediate movement, without prior stillness.
      for (int n = 0; n < 65; ++n) {
        float gyro[3] = {bias[0], bias[1], bias[2]};
        gyro[axis] += sign * 3.0f;
        continuous.update(gyro, false, now += 20);
      }
      assert(continuous.axis() == axis && continuous.repetitionCount() == 0);
      for (int n = 0; n < 35; ++n) {
        float gyro[3] = {bias[0], bias[1], bias[2]};
        gyro[axis] -= sign * 3.0f;
        continuous.update(gyro, false, now += 20);
      }
      for (int n = 0; n < 25; ++n) continuous.update(bias, true, now += 20);
      assert(continuous.repetitionCount() == 1); // First valid complete cycle.
      assert(continuous.consumeRepEvent());
      assert(continuous.calibrationCount() == 0 && !continuous.consumeCalibrationEvent());
      for (int n = 0; n < 100; ++n) continuous.update(bias, true, now += 20);
      assert(continuous.repetitionCount() == 1);
    }
  }
  const PauseCalibration squat = ExercisePresets::pause(SQUAT);
  const PauseCalibration lunge = ExercisePresets::pause(LUNGE);
  assert(squat.highThreshold == 0.80f && squat.lowThreshold == 0.30f && squat.requiredPeak == 1.00f);
  assert(squat.highThreshold == lunge.highThreshold && squat.lowThreshold == lunge.lowThreshold);
  assert(squat.requiredPeak == lunge.requiredPeak && squat.minDuration == lunge.minDuration);
  assert(squat.maxDuration == lunge.maxDuration);
  assert(!ExercisePresets::allowed(HIP, EXERCISE_COUNT));
  // A reset discards an incomplete movement rather than completing it later.
  RepCounter resetCounter(getGenericSettings());
  assert(resetCounter.beginPreset(now, squat));
  for (int n = 0; n < 25; ++n)
    resetCounter.update(GeneralConfig::GRAVITY_MPS2 + 1.5f, now += 20);
  assert(resetCounter.beginPreset(now, ExercisePresets::pause(PUSHUP)));
  for (int n = 0; n < 30; ++n)
    resetCounter.update(GeneralConfig::GRAVITY_MPS2, now += 20);
  assert(resetCounter.repetitionCount() == 0 && !resetCounter.consumeRepEvent());
  movement(resetCounter, now, GeneralConfig::GRAVITY_MPS2, 0.8f);
  assert(resetCounter.repetitionCount() == 1);
  PauseCalibration invalid;
  assert(!resetCounter.beginPreset(now, invalid));
  assert(resetCounter.state() != REP_COUNTING);
  puts("Exercise preset tests passed");
}
