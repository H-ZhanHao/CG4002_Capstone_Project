#include "NodeSession.h"

namespace {
  constexpr uint32_t MAGIC = 0x45584E32;
  // Deliberately incompatible with the old gateway/telemetry protocol.
  constexpr uint8_t VERSION = 4;
}

NodeSession::NodeSession(Send send, Log log) : send_(send), log_(log) {}

void NodeSession::begin(NodeRole role, uint32_t boot, CountingMode mode) {
  role_ = role;
  boot_ = boot ? boot : 1;
  request_ = requestedAt_ = lastAttempt_ = 0;
  attempts_ = 0;
  state_ = State::STANDBY;
  restart_ = false;
  mode_ = mode;
  selectedMode_ = mode;
  modeRequest_ = modeRequestedAt_ = modeLastAttempt_ = 0;
  peerModeBoot_ = peerModeRequest_ = 0;
  modeAttempts_ = 0;
  modePending_ = peerModeAccepted_ = false;
  activePeerBoot_ = ledSendSequence_ = 0;
  ledPeerBoot_ = ledPeerRequest_ = ledReceiveSequence_ = 0;
  ledStartedAt_ = ledDuration_ = 0;
  ledReadyPattern_ = false;
}

void NodeSession::applyLedFeedback(LedFeedback feedback, uint32_t now) {
  ledStartedAt_ = now;
  ledReadyPattern_ = feedback == LedFeedback::READY;
  ledDuration_ = feedback == LedFeedback::READY ? LedTiming::READY_MS
               : feedback == LedFeedback::REP ? LedTiming::REP_MS : 0;
}

bool NodeSession::feedbackLedOn(uint32_t now) const {
  const uint32_t elapsed = now - ledStartedAt_;
  if (!ledDuration_ || elapsed >= ledDuration_) return false;
  // Start with a brief OFF interval so all three blinks are visible even if
  // the active node's calibration LED was already lit. No blocking delays.
  return !ledReadyPattern_ || (elapsed / LedTiming::READY_STEP_MS) % 2 == 1;
}

void NodeSession::showLedFeedback(LedFeedback feedback, uint32_t now) {
  if (!localActive() || !activePeerBoot_) return;
  if (feedback != LedFeedback::CLEAR && feedback != LedFeedback::READY && feedback != LedFeedback::REP) return;
  applyLedFeedback(feedback, now);
  ++ledSendSequence_;
  if (!ledSendSequence_) ++ledSendSequence_;
  LinkPacket p = packet(PacketKind::LED_FEEDBACK, request_, activePeerBoot_);
  p.sequence = ledSendSequence_;
  p.mode = static_cast<uint8_t>(feedback);
  send_(p); // Best effort: a missed visual event never stalls local counting.
}

void NodeSession::clearLedFeedback(uint32_t now) {
  if (localActive() && ledSendSequence_) showLedFeedback(LedFeedback::CLEAR, now);
  ledDuration_ = 0;
}

void NodeSession::applyMode(CountingMode mode) {
  if (mode_ == mode) return;
  mode_ = mode;
  ledDuration_ = 0;
  if (waiting()) state_ = State::STANDBY;
  restart_ = true;
  log_(mode == PAUSE_MODE ? "[Mode: PAUSE | Session restarted; activation unchanged]"
                         : "[Mode: CONTINUOUS | Session restarted; activation unchanged]");
}

void NodeSession::setMode(CountingMode mode, uint32_t now) {
  if (mode != PAUSE_MODE && mode != CONTINUOUS_MODE) return;
  if (!localActive()) {
    log_("[Mode command rejected: use the active node's monitor, or ACTIVATE this node first]");
    return;
  }
  selectedMode_ = mode;
  applyMode(mode);
  ++modeRequest_;
  if (!modeRequest_) ++modeRequest_;
  modeRequestedAt_ = now;
  modeAttempts_ = 0;
  modePending_ = true;
  log_("[Mode sync requested; waiting for peer acknowledgement]");
  requestMode(now);
}

void NodeSession::selectExerciseMode(CountingMode mode, uint32_t now) {
  if (mode != PAUSE_MODE && mode != CONTINUOUS_MODE) return;
  if (waiting()) standby();
  selectedMode_ = mode;
  if (localActive()) setMode(mode, now);
  // An exercise change resets the counter even when its mode is unchanged.
  // Coalesce the exercise and mode changes into one baseline restart.
  restart_ = true;
}

void NodeSession::requestMode(uint32_t now) {
  modeLastAttempt_ = now;
  ++modeAttempts_;
  LinkPacket p = packet(PacketKind::SET_MODE, modeRequest_, 0);
  p.mode = mode_;
  send_(p);
}

void NodeSession::expireMode(uint32_t now) {
  if (!modePending_ || now - modeRequestedAt_ < LinkTiming::REQUEST_TIMEOUT_MS) return;
  modePending_ = false;
  log_("[Mode sync unconfirmed: local mode retained; check peer power/link and resend MODE command]");
}

LinkPacket NodeSession::packet(PacketKind kind, uint32_t request, uint32_t targetBoot) const {
  LinkPacket result = {};
  result.magic = MAGIC;
  result.version = VERSION;
  result.kind = kind;
  result.boot = boot_;
  result.targetBoot = targetBoot;
  result.request = request;
  result.role = role_;
  return result;
}

void NodeSession::activate(uint32_t now) {
  expire(now);
  if (localActive()) {
    log_("[Already active; send RESET to clear reps and restart baseline]");
    return;
  }
  if (waiting()) {
    log_("[Activation already pending; waiting for peer standby]");
    return;
  }
  ledDuration_ = 0;
  activePeerBoot_ = ledSendSequence_ = 0;
  ledPeerBoot_ = ledPeerRequest_ = ledReceiveSequence_ = 0;
  ++request_;
  if (!request_) ++request_;
  requestedAt_ = now;
  attempts_ = 0;
  restart_ = false;
  state_ = State::WAITING;
  log_("[Activation requested; waiting for the other node to acknowledge standby]");
  requestStandby(now);
}

void NodeSession::standby() {
  // CLEAR has no pulse duration, so its timestamp does not affect output.
  clearLedFeedback(0);
  state_ = State::STANDBY;
  modePending_ = false;
  restart_ = false;
  log_("[Standby; rep counting stopped. Send ACTIVATE to start]");
}

void NodeSession::requestStandby(uint32_t now) {
  lastAttempt_ = now;
  ++attempts_;
  send_(packet(PacketKind::DEACTIVATE, request_, 0));
}

bool NodeSession::expire(uint32_t now) {
  if (!waiting() || now - requestedAt_ < LinkTiming::REQUEST_TIMEOUT_MS) return false;
  state_ = State::FAILED;
  restart_ = false;
  log_("[Activation failed: other node did not acknowledge standby. Check its power/link; send ACTIVATE to retry]");
  return true;
}

void NodeSession::receive(const LinkPacket& p, uint32_t now) {
  if (p.magic != MAGIC || p.version != VERSION || !p.boot || !p.request) return;
  const NodeRole peer = role_ == HIP ? WRIST : HIP;
  if (p.role != peer) return;

  // Check the deadline here too: a delayed ACK after a blocked main loop must
  // not activate a request whose timeout has already elapsed.
  expire(now);
  expireMode(now);
  if (p.kind == PacketKind::LED_FEEDBACK) {
    if (state_ != State::STANDBY || p.targetBoot != boot_ ||
        p.boot != ledPeerBoot_ || p.request != ledPeerRequest_ || !p.sequence) return;
    if (p.mode < static_cast<uint8_t>(LedFeedback::CLEAR) ||
        p.mode > static_cast<uint8_t>(LedFeedback::REP)) return;
    if (ledReceiveSequence_ && (int32_t)(p.sequence - ledReceiveSequence_) <= 0) return;
    ledReceiveSequence_ = p.sequence;
    applyLedFeedback(static_cast<LedFeedback>(p.mode), now);
    return; // No acknowledgement, retry or effect on reps / mode / activation.
  }
  if (p.sequence != 0) return;
  if (p.kind == PacketKind::SET_MODE || p.kind == PacketKind::MODE_ACK) {
    if (p.mode != PAUSE_MODE && p.mode != CONTINUOUS_MODE) return;
    if (p.kind == PacketKind::MODE_ACK) {
      if (modePending_ && p.targetBoot == boot_ && p.request == modeRequest_ && p.mode == mode_) {
        modePending_ = false;
        log_(mode_ == PAUSE_MODE ? "[Mode synced: PAUSE | Peer acknowledged]"
                                : "[Mode synced: CONTINUOUS | Peer acknowledged]");
      }
      return;
    }
    if (p.targetBoot != 0) return;
    // The active node owns mode selection. Ignore delayed commands from the
    // previously active peer after an activation handover.
    if (localActive()) return;
    if (p.boot == peerModeBoot_) {
      if (p.request != peerModeRequest_ && (int32_t)(p.request - peerModeRequest_) <= 0) return;
      if (p.request == peerModeRequest_) {
        // Retry after a lost ACK: acknowledge without resetting calibration.
        if (!peerModeAccepted_ || p.mode != mode_) return;
        LinkPacket ack = packet(PacketKind::MODE_ACK, p.request, p.boot);
        ack.mode = p.mode;
        send_(ack);
        return;
      }
    }
    peerModeBoot_ = p.boot;
    peerModeRequest_ = p.request;
    peerModeAccepted_ = false;
    modePending_ = false;
    applyMode(static_cast<CountingMode>(p.mode));
    peerModeAccepted_ = true;
    log_("[Mode received from peer]");
    LinkPacket ack = packet(PacketKind::MODE_ACK, p.request, p.boot);
    ack.mode = p.mode;
    send_(ack);
    return;
  }
  if (p.mode != 0) return;
  if (p.kind == PacketKind::DEACTIVATE && p.targetBoot == 0) {
    // If both request activation together, the hip's pending request wins.
    // The wrist cancels its request BEFORE acknowledging the hip. Ignoring
    // the competing request here ensures both cannot accept each other's ACK.
    if (role_ == HIP && waiting()) return;
    if (p.boot != ledPeerBoot_ || (int32_t)(p.request - ledPeerRequest_) > 0) {
      ledPeerBoot_ = p.boot;
      ledPeerRequest_ = p.request;
      ledReceiveSequence_ = 0;
    }
    ledDuration_ = 0;
    activePeerBoot_ = ledSendSequence_ = 0;
    state_ = State::STANDBY;
    modePending_ = false;
    restart_ = false;
    log_(role_ == HIP ? "[Hip in standby for wrist activation]"
                     : "[Wrist in standby for hip activation]");
    // Even a duplicate request must stop us before re-acknowledging: we might
    // have received another local ACTIVATE since the previous acknowledgement.
    send_(packet(PacketKind::STANDBY_ACK, p.request, p.boot));
  } else if (p.kind == PacketKind::STANDBY_ACK && waiting() &&
             p.targetBoot == boot_ && p.request == request_) {
    state_ = State::ACTIVE;
    activePeerBoot_ = p.boot;
    ledDuration_ = 0;
    restart_ = true;
    log_(role_ == HIP ? "[Hip active; wrist acknowledged standby. Starting baseline]"
                     : "[Wrist active; hip acknowledged standby. Starting baseline]");
    // Apply only after the peer acknowledges standby. Even equal modes must
    // synchronize: the peer may have rebooted or missed the previous update.
    setMode(selectedMode_, now);
  }
}

void NodeSession::update(uint32_t now) {
  expireMode(now);
  if (modePending_ && modeAttempts_ < LinkTiming::MAX_ATTEMPTS &&
      now - modeLastAttempt_ >= LinkTiming::RETRY_MS) requestMode(now);
  if (expire(now) || !waiting()) return;
  if (attempts_ < LinkTiming::MAX_ATTEMPTS && now - lastAttempt_ >= LinkTiming::RETRY_MS) {
    requestStandby(now);
  }
}

bool NodeSession::consumeRestart() {
  const bool result = restart_;
  restart_ = false;
  return result;
}

TelemetryData NodeSession::output(const TelemetryData& local) const {
  TelemetryData result = local;
  result.source = role_;
  if (state_ == State::WAITING) result.repState = WAITING_FOR_PEER;
  else if (state_ == State::FAILED) result.repState = ACTIVATION_FAILED;
  else if (state_ == State::STANDBY) result.repState = NODE_STANDBY;
  return result;
}

void NodeSession::status() const {
  log_(role_ == HIP ? "[Role: HIP]" : "[Role: WRIST]");
  if (localActive()) {
    log_(mode_ == PAUSE_MODE ? "[Mode: PAUSE]" : "[Mode: CONTINUOUS]");
  } else {
    log_(selectedMode_ == PAUSE_MODE ? "[Mode on activation: PAUSE]"
                                   : "[Mode on activation: CONTINUOUS]");
  }
  if (modePending_) log_("[Mode sync pending]");
  if (localActive()) log_("[Active | Other node acknowledged standby at activation]");
  else if (waiting()) log_("[Waiting for peer standby acknowledgement]");
  else if (state_ == State::FAILED) log_("[Activation failed | Inactive | Send ACTIVATE to retry]");
  else log_("[Standby | Send ACTIVATE to start]");
  // With no heartbeat, STATUS cannot claim that the peer is currently online.
}
