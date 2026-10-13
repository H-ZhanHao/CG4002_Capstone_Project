#pragma once

#include <ctype.h>
#include <string.h>
#include "NodeTypes.h"

enum ExerciseType {
  PUSHUP = 0, SITUP, SQUAT, LUNGE, LEG_RAISE, JUMPING_JACK, EXERCISE_COUNT
};

inline const char* exerciseName(ExerciseType exercise) {
  static const char* const names[] = {
    "PUSHUP", "SITUP", "SQUAT", "LUNGE", "LEG_RAISE", "JUMPING_JACK"
  };
  return exercise >= PUSHUP && exercise < EXERCISE_COUNT
    ? names[exercise] : "UNKNOWN";
}

inline bool parseExercise(const char* text, ExerciseType& exercise) {
  char normalized[24];
  unsigned int length = 0;
  for (; *text; ++text) {
    if (*text == '_' || *text == '-' || isspace((unsigned char)*text)) continue;
    if (length + 1 >= sizeof(normalized)) return false;
    normalized[length++] = (char)toupper((unsigned char)*text);
  }
  normalized[length] = '\0';
  if (length && normalized[length - 1] == 'S') normalized[--length] = '\0';
  static const char* const names[] = {
    "PUSHUP", "SITUP", "SQUAT", "LUNGE", "LEGRAISE", "JUMPINGJACK"
  };
  for (int i = 0; i < EXERCISE_COUNT; ++i) {
    if (!strcmp(normalized, names[i])) {
      exercise = (ExerciseType)i;
      return true;
    }
  }
  return false;
}

// Only learned movement limits are retained, never baseline, counts or cycles.
struct PauseCalibration {
  bool valid = false;
  float highThreshold = 0, lowThreshold = 0, requiredPeak = 0;
  unsigned long minDuration = 0, maxDuration = 0;
};

struct ContinuousCalibration {
  bool valid = false;
  int axis = -1;
  float firstSign = 0;
  float startGate = 0, endGate = 0, requiredAngle = 0, requiredPeak = 0;
  unsigned long minDuration = 0, maxDuration = 0;
};

class ExerciseProfiles {
 public:
  PauseCalibration& pause(ExerciseType exercise) { return pause_[exercise]; }
  ContinuousCalibration& continuous(ExerciseType exercise) { return continuous_[exercise]; }
  bool ready(ExerciseType exercise, CountingMode mode) const {
    return mode == PAUSE_MODE ? pause_[exercise].valid : continuous_[exercise].valid;
  }
  void clear(ExerciseType exercise, CountingMode mode) {
    if (mode == PAUSE_MODE) pause_[exercise] = PauseCalibration{};
    else continuous_[exercise] = ContinuousCalibration{};
  }

 private:
  PauseCalibration pause_[EXERCISE_COUNT];
  ContinuousCalibration continuous_[EXERCISE_COUNT];
};
