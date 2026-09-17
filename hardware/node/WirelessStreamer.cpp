#include "WirelessStreamer.h"

WirelessStreamer::WirelessStreamer()
  : lastOutputTime_(0) {
}

bool WirelessStreamer::begin(
  const char* deviceName
) {
  return bluetooth_.begin(
    deviceName
  );
}

void WirelessStreamer::update(
  float rawMagnitude,
  float filteredMagnitude,
  const RepCounter& repCounter,
  const FallDetector& fallDetector,
  bool buzzerActive,
  ExerciseType selectedExercise,
  unsigned long currentTime
) {
  // Transmit at 10 Hz
  if (
    currentTime - lastOutputTime_ <
    OUTPUT_INTERVAL_MS
  ) {
    return;
  }

  lastOutputTime_ = currentTime;

  // Do not transmit unless a device is connected
  if (!bluetooth_.hasClient()) {
    return;
  }

  bluetooth_.print("Raw:");
  bluetooth_.print(
    rawMagnitude,
    3
  );

  bluetooth_.print("\tFiltered:");
  bluetooth_.print(
    filteredMagnitude,
    3
  );

  bluetooth_.print("\tMotion:");
  bluetooth_.print(
    repCounter.motionSignal(),
    3
  );

  bluetooth_.print("\tReps:");
  bluetooth_.print(
    repCounter.repetitionCount()
  );

  bluetooth_.print("\tCalibrationReps:");
  bluetooth_.print(
    repCounter.calibrationCount()
  );

  bluetooth_.print("\tRepState:");
  bluetooth_.print(
    (int)repCounter.state()
  );

  bluetooth_.print("\tFallState:");
  bluetooth_.print(
    (int)fallDetector.state()
  );

  bluetooth_.print("\tBuzzer:");
  bluetooth_.print(
    buzzerActive ? 1 : 0
  );

  bluetooth_.print("\tExercise:");
  bluetooth_.println(
    (int)selectedExercise
  );
}