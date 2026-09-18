#include "WirelessStreamer.h"

void printTelemetry(
  Print& output,
  const TelemetryData& data
) {
  output.print("Raw:");
  output.print(data.raw, 3);

  output.print("\tFiltered:");
  output.print(data.filtered, 3);

  output.print("\tMotion:");
  output.print(data.motion, 3);

  output.print("\tHigh:");
  output.print(data.high, 3);

  output.print("\tLow:");
  output.print(data.low, 3);

  output.print("\tRequiredPeak:");
  output.print(data.requiredPeak, 3);

  output.print("\tLastPeak:");
  output.print(data.lastPeak, 3);

  output.print("\tLastTimeS:");
  output.print(data.lastTimeS, 2);

  output.print("\tGyro:");
  output.print(data.gyroSignal, 3);

  output.print("\tGyroHigh:");
  output.print(data.gyroHigh, 3);

  output.print("\tRequiredAngle:");
  output.print(data.requiredAngle, 3);

  output.print("\tLastAngle:");
  output.print(data.lastAngle, 3);

  output.print("\tCycleTimeS:");
  output.print(data.cycleTimeS, 2);

  output.print("\tReps:");
  output.print(data.reps);

  output.print("\tCalibrationReps:");
  output.print(data.calibrationReps);

  output.print("\tPauseCandidates:");
  output.print(data.pauseCandidates);

  output.print("\tContinuousCandidates:");
  output.print(data.continuousCandidates);

  output.print("\tRepState:");
  output.print(data.repState);

  output.print("\tMode:");
  output.print(data.mode);

  output.print("\tRequestedMode:");
  output.print(data.requestedMode);

  output.print("\tGyroAxis:");
  output.print(data.gyroAxis);

  output.print("\tFallState:");
  output.print(data.fallState);

  output.print("\tBuzzer:");
  output.print(data.buzzer ? 1 : 0);

  output.print("\tExercise:");
  output.print(data.exercise);

  output.print("\tRecognitionActive:");
  output.print(data.recognitionActive ? 1 : 0);

  output.print("\tRecognitionMock:");
  output.println(data.recognitionMock ? 1 : 0);
}

WirelessStreamer::WirelessStreamer()
  : lastOutputTime_(0) {
}

bool WirelessStreamer::begin(const char* deviceName) {
  lastOutputTime_ = 0;
  return bluetooth_.begin(deviceName);
}

void WirelessStreamer::update(
  const TelemetryData& data,
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

  printTelemetry(bluetooth_, data);
}