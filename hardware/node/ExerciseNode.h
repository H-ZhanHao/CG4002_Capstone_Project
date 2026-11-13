#pragma once

#include "NodeTypes.h"
#include "SensorHistory.h"
#include "GenericPeakDetector.h"
#include "WindowScheduler.h"
#include "StreamCoordination.h"
#include "CommsRepCounter.h"
#include "CommsActivityState.h"

// Shared implementation for the two independently uploadable ESP-IDF targets.
namespace ExerciseNode {
  void begin(NodeRole role);
  void update();
  // Apply a cumulative AR result via comms in the main loop, never a callback.
  // Use the HW-issued boot/session from REPS or STATUS, after ACTIVATE completes.
  CommsRepResult receiveRepCount(uint32_t boot, uint32_t session, uint32_t total);
  // Main-loop activity API, independent of ACTIVATE/STANDBY and rep resets.
  // Use the boot ID and last sequence from ACTIVITY/STATUS; retry identically.
  ActivityResult receiveActivity(uint32_t boot, uint32_t sequence, UserActivity state);
  ActivityStatus activityStatus();
  // Call in the same main loop as update(); copy owns its data after return.
  SensorWindowResult copySensorWindow(uint32_t endMs, SensorSample* destination, size_t capacity);
  // Optional diagnostic observer, separate from coordinated trigger requests.
  // One pending observation, preserving the first until consumed.
  bool consumeGenericPeak(GenericPeakEvent& event);
  // All window stream APIs run in the same main loop as update(). No transport calls.
  bool startWindowStream(uint32_t t0); // Explicit local bench override, coordinator=0.
  void stopWindowStream();
  void armWindowStream();
  WindowStreamStatus windowStreamStatus();
  // Caller owns the copied snapshot. On ESP32, allocate destination statically.
  bool takeSensorWindow(ScheduledWindow& destination);
  // Comms adapter: queue inbound messages to the main loop before calling these.
  // poll retries the same first peak every 500 ms until a start/stop is accepted.
  bool pollStreamTrigger(StreamTrigger& trigger);
  StreamNodeIdentity streamIdentity();
  StreamClockReply streamClockReply(uint32_t probe, uint32_t receivedMs);
  StreamJoinResult joinWindowStream(const StreamStart& start);
  bool endWindowStream(uint32_t coordinator, uint32_t session);
}
