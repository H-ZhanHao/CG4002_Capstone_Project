#pragma once

#include "GenericPeakDetector.h"
#include "NodeTypes.h"
#include "WindowScheduler.h"

namespace StreamCoordinationConfig {
  constexpr uint32_t RETRY_MS = 500;
  constexpr uint32_t CLOCK_MAX_AGE_MS = 30000;
  constexpr uint32_t MAX_CLOCK_NETWORK_RTT_MS = 40;
}

struct StreamNodeIdentity {
  NodeRole role = HIP;
  uint32_t boot = 0, arm = 0;
};

struct StreamTrigger {
  StreamNodeIdentity node;
  uint32_t peakMs = 0, detectedMs = 0; // Source node's local clock.
};

struct StreamStart {
  StreamNodeIdentity node; // Target boot and arm generation.
  uint32_t coordinator = 0, session = 0;
  uint32_t localT0 = 0; // Already translated into the target node's clock.
};

struct StreamClockReply {
  StreamNodeIdentity node;
  uint32_t probe = 0, receivedMs = 0, sentMs = 0;
  bool armed = false;
};

enum class StreamJoinResult { STARTED, ALREADY_RUNNING, STALE_TARGET, STOPPED, BUSY, CONFLICT, INVALID };
const char* streamJoinResultName(StreamJoinResult result);

// Node-side control. All methods run in the sampling/main loop, never callbacks.
// Owns no sample storage: uses the existing history and bounded scheduler queue.
class NodeStreamControl {
 public:
  NodeStreamControl(WindowScheduler& scheduler, const SensorHistory& history)
    : scheduler_(scheduler), history_(history) {}
  void begin(NodeRole role, uint32_t boot);
  StreamNodeIdentity identity() const { return identity_; }
  bool peak(const GenericPeakEvent& event); // Preserve the first request until start/stop.
  bool pendingTrigger(StreamTrigger& trigger) const;
  bool pollTrigger(uint32_t now, StreamTrigger& trigger); // Retry the SAME request.
  StreamJoinResult join(const StreamStart& start, uint32_t now);
  bool end(uint32_t coordinator, uint32_t session); // Scoped, idempotent stop.
  bool startLocal(uint32_t t0, uint32_t now); // Explicit bench override only.
  void stop(); // Cancel requests, invalidate old starts, clear queued windows.
  void arm(); // A running session is unchanged.
  StreamClockReply clockReply(uint32_t probe, uint32_t received, uint32_t sent) const;

 private:
  WindowScheduler& scheduler_;
  const SensorHistory& history_;
  StreamNodeIdentity identity_;
  StreamTrigger trigger_;
  StreamStart accepted_;
  bool pending_ = false, attempted_ = false;
  uint32_t lastAttempt_ = 0;
};

enum class StreamTriggerResult { CREATED, EXISTING, NOT_READY, STALE_TARGET, INVALID };

// Portable comms-side coordinator; NOT instantiated in the node firmware.
// One serialized owner calls these methods. Transport supplies correlated clock
// replies, carries triggers/starts/acks, and stops/rearms both nodes after close().
// Timestamps use uint32 modular arithmetic, with intervals strictly below 2^31 ms.
class StreamCoordinator {
 public:
  void begin(uint32_t coordinatorBoot); // Fresh nonzero random ID on coordinator reboot.
  // Four timestamps: comms send, node receive/send, comms receive. Caller must
  // match reply.probe to its outstanding probe on the current node connection.
  bool observeClock(const StreamClockReply& reply, uint32_t sent, uint32_t received);
  bool ready(NodeRole role, uint32_t now) const;
  uint32_t clockUncertaintyMs(NodeRole role) const;
  StreamTriggerResult trigger(const StreamTrigger& trigger, uint32_t now);
  bool getStart(NodeRole role, StreamStart& start) const;
  bool pollStart(NodeRole role, uint32_t now, StreamStart& start);
  bool acknowledge(const StreamStart& start, StreamJoinResult result);
  bool bothStarted() const { return active_ && acknowledged_[0] && acknowledged_[1]; }
  bool close(uint32_t coordinator, uint32_t session);

 private:
  struct Clock {
    StreamNodeIdentity node;
    uint32_t offset = 0, measuredAt = 0, uncertainty = 0;
    bool valid = false, needsRearm = false;
  } clocks_[2];
  StreamStart starts_[2];
  bool acknowledged_[2] = {}, attempted_[2] = {};
  uint32_t lastAttempt_[2] = {};
  uint32_t coordinator_ = 0, session_ = 0;
  bool active_ = false;
};
