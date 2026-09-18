#pragma once

#include <Arduino.h>

#include "ExerciseConfig.h"

// Supplies an exercise label during movement calibration.
//
// The current implementation is a mock.
// It does not classify the sensor readings.
class ExerciseRecognition {
 public:
  ExerciseRecognition();

  // Clear the label and allow recognition for this session.
  void beginCalibration(unsigned long currentTime);

  // Called for sensor samples belonging to movement calibration.
  void update(
    const float acceleration[3],
    const float gyroscope[3],
    int calibrationCandidates,
    unsigned long currentTime
  );

  // Freeze the label when movement calibration ends.
  void endCalibration();

  // Integration point for a future external recognizer.
  // Accepts the first valid label during calibration.
  // Returns false for UNKNOWN, invalid, late or duplicate results.
  bool submitResult(ExerciseType exercise);

  ExerciseType exercise() const;
  bool active() const;
  bool usingMock() const;

 private:
  bool active_;
  unsigned long calibrationStartTime_;

  ExerciseType exercise_;
};