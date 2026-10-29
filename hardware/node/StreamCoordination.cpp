#include "StreamCoordination.h"

namespace {
constexpr uint32_t HALF_RANGE = 0x80000000u;
int indexOf(NodeRole role) { return role == HIP ? 0 : role == WRIST ? 1 : -1; }
bool sameNode(const StreamNodeIdentity& a, const StreamNodeIdentity& b) {
  return a.role == b.role && a.boot == b.boot && a.arm == b.arm;
}
bool validNode(const StreamNodeIdentity& node) {
  return indexOf(node.role) >= 0 && node.boot && node.arm;
}
bool sameStart(const StreamStart& a, const StreamStart& b) {
  return sameNode(a.node, b.node) && a.coordinator == b.coordinator &&
    a.session == b.session && a.localT0 == b.localT0;
}
}

const char* streamJoinResultName(StreamJoinResult result) {
  switch (result) {
    case StreamJoinResult::STARTED: return "STARTED";
    case StreamJoinResult::ALREADY_RUNNING: return "ALREADY_RUNNING";
    case StreamJoinResult::STALE_TARGET: return "STALE_TARGET";
    case StreamJoinResult::STOPPED: return "STOPPED";
    case StreamJoinResult::BUSY: return "BUSY";
    case StreamJoinResult::CONFLICT: return "CONFLICT";
    case StreamJoinResult::INVALID: return "INVALID";
  }
  return "INVALID";
}

void NodeStreamControl::begin(NodeRole role, uint32_t boot) {
  scheduler_.reset();
  identity_.role = role;
  identity_.boot = boot ? boot : 1;
  identity_.arm = 1;
  trigger_ = StreamTrigger{};
  accepted_ = StreamStart{};
  pending_ = attempted_ = false;
  lastAttempt_ = 0;
}

bool NodeStreamControl::peak(const GenericPeakEvent& event) {
  if (scheduler_.status().state != WindowStreamState::ARMED || pending_) return false;
  trigger_.node = identity_;
  trigger_.peakMs = event.peakMs;
  trigger_.detectedMs = event.detectedMs;
  pending_ = true;
  attempted_ = false;
  return true;
}

bool NodeStreamControl::pendingTrigger(StreamTrigger& trigger) const {
  if (!pending_) return false;
  trigger = trigger_;
  return true;
}

bool NodeStreamControl::pollTrigger(uint32_t now, StreamTrigger& trigger) {
  if (!pending_ || (attempted_ && now - lastAttempt_ < StreamCoordinationConfig::RETRY_MS)) return false;
  trigger = trigger_;
  attempted_ = true;
  lastAttempt_ = now;
  return true;
}

StreamJoinResult NodeStreamControl::join(const StreamStart& start, uint32_t now) {
  if (!start.coordinator || !start.session) return StreamJoinResult::INVALID;
  if (!sameNode(start.node, identity_)) return StreamJoinResult::STALE_TARGET;
  const auto state = scheduler_.status().state;
  if (state == WindowStreamState::STOPPED) return StreamJoinResult::STOPPED;
  if (state == WindowStreamState::RUNNING) {
    if (sameStart(start, accepted_)) return StreamJoinResult::ALREADY_RUNNING;
    return start.coordinator == accepted_.coordinator && start.session == accepted_.session
      ? StreamJoinResult::CONFLICT : StreamJoinResult::BUSY;
  }
  if (!scheduler_.start(start.localT0, now, history_, start.coordinator, start.session))
    return StreamJoinResult::INVALID;
  accepted_ = start;
  pending_ = false;
  return StreamJoinResult::STARTED;
}

bool NodeStreamControl::end(uint32_t coordinator, uint32_t session) {
  if (!coordinator || !session || accepted_.coordinator != coordinator || accepted_.session != session)
    return false;
  // Retransmitted END after rearming must not cancel the next pending request.
  if (scheduler_.status().state == WindowStreamState::RUNNING && sameNode(accepted_.node, identity_)) stop();
  return true;
}

bool NodeStreamControl::startLocal(uint32_t t0, uint32_t now) {
  if (!scheduler_.start(t0, now, history_)) return false;
  accepted_ = StreamStart{};
  pending_ = false;
  return true;
}

void NodeStreamControl::stop() {
  if (scheduler_.status().state != WindowStreamState::STOPPED) {
    ++identity_.arm;
    if (!identity_.arm) ++identity_.arm;
  }
  pending_ = attempted_ = false;
  scheduler_.stop();
}

void NodeStreamControl::arm() { scheduler_.arm(); }

StreamClockReply NodeStreamControl::clockReply(uint32_t probe, uint32_t received, uint32_t sent) const {
  StreamClockReply reply;
  reply.node = identity_;
  reply.probe = probe;
  reply.receivedMs = received;
  reply.sentMs = sent;
  reply.armed = scheduler_.status().state == WindowStreamState::ARMED;
  return reply;
}

void StreamCoordinator::begin(uint32_t coordinatorBoot) {
  coordinator_ = coordinatorBoot;
  session_ = 0;
  active_ = false;
  for (int i = 0; i < 2; ++i) {
    clocks_[i] = Clock{};
    starts_[i] = StreamStart{};
    acknowledged_[i] = attempted_[i] = false;
    lastAttempt_[i] = 0;
  }
}

bool StreamCoordinator::observeClock(const StreamClockReply& reply, uint32_t sent, uint32_t received) {
  if (!coordinator_ || !validNode(reply.node)) return false;
  const int i = indexOf(reply.node.role);
  Clock& clock = clocks_[i];
  if (clock.valid && received - clock.measuredAt >= HALF_RANGE) return false;
  if (active_ && !sameNode(reply.node, starts_[i].node)) return false;
  if (clock.node.boot == reply.node.boot) {
    if (clock.needsRearm && clock.node.arm == reply.node.arm) return false;
    if (reply.node.arm - clock.node.arm >= HALF_RANGE) return false;
  }
  const uint32_t roundTrip = received - sent;
  const uint32_t processing = reply.sentMs - reply.receivedMs;
  if (roundTrip >= HALF_RANGE || processing > roundTrip ||
      roundTrip - processing > StreamCoordinationConfig::MAX_CLOCK_NETWORK_RTT_MS) return false;
  if (!reply.armed) {
    clock.valid = false;
    return false;
  }
  clock.node = reply.node;
  // Compare midpoint times after removing node processing time. Delay asymmetry
  // remains uncertainty; this is an estimate, never a claim of identical clocks.
  clock.offset = (reply.receivedMs + processing / 2) - (sent + roundTrip / 2);
  clock.measuredAt = received;
  clock.uncertainty = (roundTrip - processing + 1) / 2 + 1; // Include ms rounding.
  clock.valid = true;
  clock.needsRearm = false;
  return true;
}

bool StreamCoordinator::ready(NodeRole role, uint32_t now) const {
  const int i = indexOf(role);
  return i >= 0 && clocks_[i].valid &&
    now - clocks_[i].measuredAt <= StreamCoordinationConfig::CLOCK_MAX_AGE_MS;
}

uint32_t StreamCoordinator::clockUncertaintyMs(NodeRole role) const {
  const int i = indexOf(role);
  return i >= 0 && clocks_[i].valid ? clocks_[i].uncertainty : UINT32_MAX;
}

StreamTriggerResult StreamCoordinator::trigger(const StreamTrigger& trigger, uint32_t now) {
  if (!validNode(trigger.node) || trigger.detectedMs - trigger.peakMs >= HALF_RANGE)
    return StreamTriggerResult::INVALID;
  const int source = indexOf(trigger.node.role);
  if (active_) return sameNode(trigger.node, starts_[source].node)
    ? StreamTriggerResult::EXISTING : StreamTriggerResult::STALE_TARGET;
  if (!ready(HIP, now) || !ready(WRIST, now)) return StreamTriggerResult::NOT_READY;
  if (!sameNode(trigger.node, clocks_[source].node)) return StreamTriggerResult::STALE_TARGET;
  const uint32_t coordinatorT0 = trigger.peakMs - clocks_[source].offset;
  if (now - coordinatorT0 >= HALF_RANGE || session_ == UINT32_MAX) return StreamTriggerResult::INVALID;
  ++session_;
  for (int i = 0; i < 2; ++i) {
    starts_[i].node = clocks_[i].node;
    starts_[i].coordinator = coordinator_;
    starts_[i].session = session_;
    starts_[i].localT0 = coordinatorT0 + clocks_[i].offset;
    acknowledged_[i] = attempted_[i] = false;
  }
  active_ = true;
  return StreamTriggerResult::CREATED;
}

bool StreamCoordinator::getStart(NodeRole role, StreamStart& start) const {
  const int i = indexOf(role);
  if (!active_ || i < 0) return false;
  start = starts_[i];
  return true;
}

bool StreamCoordinator::pollStart(NodeRole role, uint32_t now, StreamStart& start) {
  const int i = indexOf(role);
  if (!active_ || i < 0 || acknowledged_[i] ||
      (attempted_[i] && now - lastAttempt_[i] < StreamCoordinationConfig::RETRY_MS)) return false;
  start = starts_[i];
  attempted_[i] = true;
  lastAttempt_[i] = now;
  return true;
}

bool StreamCoordinator::acknowledge(const StreamStart& start, StreamJoinResult result) {
  const int i = indexOf(start.node.role);
  if (!active_ || i < 0 || !sameStart(start, starts_[i]) ||
      (result != StreamJoinResult::STARTED && result != StreamJoinResult::ALREADY_RUNNING)) return false;
  acknowledged_[i] = true;
  return true;
}

bool StreamCoordinator::close(uint32_t coordinator, uint32_t session) {
  if (!coordinator || !session || coordinator != coordinator_ || session != session_) return false;
  if (!active_) return true;
  active_ = false;
  for (auto& clock : clocks_) {
    clock.valid = false;
    clock.needsRearm = true; // A closed arm cannot start a new session from an old trigger.
  }
  return true;
}
