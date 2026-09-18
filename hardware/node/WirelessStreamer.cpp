#include "WirelessStreamer.h"

void printTelemetry(
  Print& output,
  float rawMagnitude,
  float filteredMagnitude,
  const RepCounter& repCounter,
  const FallDetector& fallDetector,
  const ExerciseRecognition& recognition,
  bool buzzerActive
) {
  output.print("Raw:");
  output.print(rawMagnitude, 3);

  output.print("\tFiltered:");
  output.print(filteredMagnitude, 3);

  output.print("\tMotion:");
  output.print(repCounter.motionSignal(), 3);

  output.print("\tHigh:");
  output.print(repCounter.highThreshold(), 3);

  output.print("\tLow:");
  output.print(repCounter.lowThreshold(), 3);

  output.print("\tRequiredPeak:");
  output.print(repCounter.requiredRepPeak(), 3);

  output.print("\tLastPeak:");
  output.print(repCounter.lastCompletedPeak(), 3);

  output.print("\tMinTimeS:");
  output.print(
    repCounter.minimumRepDuration() / 1000.0f,
    2
  );

  output.print("\tMaxTimeS:");
  output.print(
    repCounter.maximumRepDuration() / 1000.0f,
    2
  );

  output.print("\tLastTimeS:");
  output.print(
    repCounter.lastCompletedDuration() / 1000.0f,
    2
  );

  output.print("\tReps:");
  output.print(repCounter.repetitionCount());

  output.print("\tCalibrationReps:");
  output.print(repCounter.calibrationCount());

  output.print("\tRepState:");
  output.print((int)repCounter.state());

  output.print("\tFallState:");
  output.print((int)fallDetector.state());

  output.print("\tBuzzer:");
  output.print(buzzerActive ? 1 : 0);

  output.print("\tExercise:");
  output.print((int)recognition.exercise());

  output.print("\tRecognitionActive:");
  output.print(recognition.active() ? 1 : 0);

  output.print("\tRecognitionMock:");
  output.println(recognition.usingMock() ? 1 : 0);
}

WirelessStreamer::WirelessStreamer()
  : lastOutputTime_(0) {
}

bool WirelessStreamer::begin(
  const char* deviceName
) {
  lastOutputTime_ = 0;
  return bluetooth_.begin(deviceName);
}

void WirelessStreamer::update(
  float rawMagnitude,
  float filteredMagnitude,
  const RepCounter& repCounter,
  const FallDetector& fallDetector,
  const ExerciseRecognition& recognition,
  bool buzzerActive,
  unsigned long currentTime
) {
  if (
    currentTime - lastOutputTime_ <
    GeneralConfig::OUTPUT_INTERVAL_MS
  ) {
    return;
  }

  lastOutputTime_ = currentTime;

  if (!bluetooth_.hasClient()) {
    return;
  }

  printTelemetry(
    bluetooth_,
    rawMagnitude,
    filteredMagnitude,
    repCounter,
    fallDetector,
    recognition,
    buzzerActive
  );
}