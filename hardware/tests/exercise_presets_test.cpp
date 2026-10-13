#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ExercisePresets.h"
#include "RepCounter.h"
#include "ContinuousCounter.h"

static void baseline(RepCounter& counter, unsigned long& now, float gravity) {
  counter.begin(now);
  for (int i = 0; i < 151; ++i) counter.update(gravity, now += 20);
  assert(counter.state() == MOVEMENT_CALIBRATION);
}

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
    // Reset and reboot both load the same fixed limits, with a fresh baseline.
    for (int session = 0; session < 2; ++session) {
      const float gravity = session ? 10.2f : 9.81f;
      counter.begin(now);
      assert(!counter.loadPreset(preset)); // Baseline cannot be skipped.
      baseline(counter, now, gravity);
      assert(counter.loadPreset(preset));
      assert(counter.state() == REP_COUNTING);
      assert(counter.repetitionCount() == 0 && counter.calibrationCount() == 0);
      assert(!counter.consumeRepEvent() && !counter.consumeCalibrationEvent());
      for (int n = 0; n < 100; ++n) counter.update(gravity, now += 20);
      assert(counter.repetitionCount() == 0);
      movement(counter, now, gravity, preset.requiredPeak + 0.1f);
      assert(counter.repetitionCount() == 1); // First rep, no calibration pair.
      assert(counter.consumeRepEvent());
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
      const float bias[3] = {0.1f + session, -0.2f, 0.3f};
      const int axis = (i + session) % 3;
      const float sign = session ? -1.0f : 1.0f;
      continuous.beginPreset(bias, now, ExercisePresets::continuous(exercise));
      assert(continuous.calibrated());
      assert(continuous.axis() == -1 && continuous.calibrationCount() == 0);
      assert(continuous.repetitionCount() == 0 && !continuous.consumeRepEvent());
      for (int n = 0; n < 100; ++n) continuous.update(bias, true, now += 20);
      assert(continuous.axis() == -1 && continuous.repetitionCount() == 0);
      // Discover the axis; do not complete any cycles yet.
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
  puts("Exercise preset tests passed");
}
