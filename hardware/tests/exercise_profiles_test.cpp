#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "RepCounter.h"
#include "ContinuousCounter.h"

static void baseline(RepCounter& counter, unsigned long& now, float gravity) {
  counter.begin(now);
  for (int i = 0; i < 151; ++i) {
    now += 20;
    counter.update(gravity, now);
  }
  assert(counter.state() == MOVEMENT_CALIBRATION);
}

static void pausedRep(RepCounter& counter, unsigned long& now, float gravity, float peak) {
  for (int i = 0; i < 25; ++i) {
    now += 20;
    counter.update(gravity + peak, now);
  }
  for (int i = 0; i < 25; ++i) {
    now += 20;
    counter.update(gravity, now);
  }
}

int main() {
  const char* aliases[] = {"pushups", "sit-ups", "squats", "LUNGES", "leg raises", "jumping_jacks"};
  for (int i = 0; i < EXERCISE_COUNT; ++i) {
    ExerciseType parsed = SQUAT;
    assert(parseExercise(aliases[i], parsed) && parsed == i);
    assert(parseExercise(exerciseName((ExerciseType)i), parsed) && parsed == i);
  }
  ExerciseType unchanged = SQUAT;
  assert(!parseExercise("burpee", unchanged) && unchanged == SQUAT);
  assert(!parseExercise("", unchanged));
  assert(!parseExercise("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", unchanged));

  ExerciseProfiles profiles;
  RepCounter pause(getGenericSettings());
  ContinuousCounter continuous;
  unsigned long now = 0;
  const float bias[3] = {0.1f, 0.2f, 0.3f};
  for (int i = 0; i < EXERCISE_COUNT; ++i) {
    const ExerciseType exercise = (ExerciseType)i;
    assert(!profiles.ready(exercise, PAUSE_MODE));
    assert(!profiles.ready(exercise, CONTINUOUS_MODE));
    baseline(pause, now, 9.81f);
    pausedRep(pause, now, 9.81f, 2.0f + i);
    assert(pause.calibrationCount() == 1);
    assert(!pause.calibration().valid);
    pausedRep(pause, now, 9.81f, 2.0f + i);
    assert(pause.state() == REP_COUNTING);
    profiles.pause(exercise) = pause.calibration();
    assert(profiles.ready(exercise, PAUSE_MODE));
    assert(!profiles.ready(exercise, CONTINUOUS_MODE));

    continuous.begin(bias, now, false);
    for (int sample = 0; sample < 1000 && !continuous.calibrated(); ++sample) {
      now += 20;
      float gyro[3] = {bias[0], bias[1], bias[2]};
      gyro[i % 3] += (3.0f + i * 0.3f) * sinf(sample * 0.02f * 6.2831853f);
      continuous.update(gyro, false, now);
    }
    assert(continuous.calibrated());
    profiles.continuous(exercise) = continuous.calibration();
    assert(profiles.ready(exercise, CONTINUOUS_MODE));
  }

  // Return to every exercise after all others have been learned. Baselines,
  // sample clocks, bias and rep counts must belong to the new session.
  for (int i = 0; i < EXERCISE_COUNT; ++i) {
    const ExerciseType exercise = (ExerciseType)i;
    pause.begin(now);
    assert(!pause.restoreCalibration(profiles.pause(exercise)));
    baseline(pause, now, 10.2f);
    assert(pause.restoreCalibration(profiles.pause(exercise)));
    assert(fabsf(pause.highThreshold() - (2.0f + i) * 0.55f) < 0.001f);
    assert(pause.repetitionCount() == 0);
    assert(!pause.consumeRepEvent());
    pausedRep(pause, now, 10.2f, 2.0f + i);
    assert(pause.repetitionCount() == 1);

    const float newBias[3] = {1.0f, -1.5f, 2.0f};
    continuous.begin(newBias, now, false);
    assert(continuous.restoreCalibration(profiles.continuous(exercise)));
    assert(continuous.axis() == i % 3);
    assert(continuous.repetitionCount() == 0);
    for (int sample = 0; sample < 100; ++sample) {
      now += 20;
      continuous.update(newBias, true, now);
    }
    assert(continuous.repetitionCount() == 0);
    for (int sample = 0; sample < 250; ++sample) {
      now += 20;
      float gyro[3] = {newBias[0], newBias[1], newBias[2]};
      gyro[i % 3] += (3.0f + i * 0.3f) * sinf(sample * 0.02f * 6.2831853f);
      continuous.update(gyro, false, now);
    }
    assert(continuous.repetitionCount() >= 3);
  }

  profiles.clear(PUSHUP, PAUSE_MODE);
  assert(!profiles.ready(PUSHUP, PAUSE_MODE));
  assert(profiles.ready(PUSHUP, CONTINUOUS_MODE));
  for (int i = 1; i < EXERCISE_COUNT; ++i) {
    assert(profiles.ready((ExerciseType)i, PAUSE_MODE));
    assert(profiles.ready((ExerciseType)i, CONTINUOUS_MODE));
  }
  profiles.clear(SITUP, CONTINUOUS_MODE);
  assert(profiles.ready(SITUP, PAUSE_MODE));
  assert(!profiles.ready(SITUP, CONTINUOUS_MODE));
  ExerciseProfiles rebooted;
  assert(!rebooted.ready(SQUAT, PAUSE_MODE));
  assert(!rebooted.ready(SQUAT, CONTINUOUS_MODE));
  puts("Exercise profile tests passed");
}
