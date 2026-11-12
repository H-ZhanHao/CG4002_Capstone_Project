#pragma once

#include "RepCounter.h"
#include "ContinuousCounter.h"

// Retained for local-detector bench tests. Not built or instantiated by firmware.
// Owns filtering and detector state; callers decide how to display returned events.
class LocalRepCounting {
 public:
  using Log = void (*)(const char*);
  bool begin(ExerciseType exercise, CountingMode mode, unsigned long now);
  bool update(float rawMagnitude, const float gyro[3], unsigned long now, Log log = nullptr);
  int count() const;
  int axis() const { return continuousCounter.axis(); }

 private:
  ExerciseSettings genericSettings = getGenericSettings();
  RepCounter repCounter{genericSettings};
  ContinuousCounter continuousCounter;
  CountingMode activeMode = PAUSE_MODE;
  float filteredMagnitude = 0;
  bool filterInitialised = false;
  void reportRepResult(RepResult result, Log log);
};
