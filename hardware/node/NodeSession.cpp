#include "NodeSession.h"

namespace {
  constexpr uint32_t MAGIC = 0x45584E32;
  // Deliberately incompatible with the old gateway/telemetry protocol.
  constexpr uint8_t VERSION = 2;
}

NodeSession::NodeSession(Send send, Log log) : send_(send), log_(log) {}

void NodeSession::begin(NodeRole role, uint32_t boot) {
  role_ = role;
  boot_ = boot ? boot : 1;
  request_ = requestedAt_ = lastAttempt_ = 0;
  attempts_ = 0;
  state_ = State::STANDBY;
  restart_ = false;
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
    log_("[Already active; send RESET to restart calibration]");
    return;
  }
  if (waiting()) {
    log_("[Activation already pending; waiting for peer standby]");
    return;
  }
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
  state_ = State::STANDBY;
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
  if (p.magic != MAGIC || p.version != VERSION || !p.boot || !p.request || p.reserved) return;
  const NodeRole peer = role_ == HIP ? WRIST : HIP;
  if (p.role != peer) return;

  // Check the deadline here too: a delayed ACK after a blocked main loop must
  // not activate a request whose timeout has already elapsed.
  expire(now);
  if (p.kind == PacketKind::DEACTIVATE && p.targetBoot == 0) {
    // If both request activation together, the hip's pending request wins.
    // The wrist cancels its request BEFORE acknowledging the hip. Ignoring
    // the competing request here ensures both cannot accept each other's ACK.
    if (role_ == HIP && waiting()) return;
    state_ = State::STANDBY;
    restart_ = false;
    log_(role_ == HIP ? "[Hip in standby for wrist activation]"
                     : "[Wrist in standby for hip activation]");
    // Even a duplicate request must stop us before re-acknowledging: we might
    // have received another local ACTIVATE since the previous acknowledgement.
    send_(packet(PacketKind::STANDBY_ACK, p.request, p.boot));
  } else if (p.kind == PacketKind::STANDBY_ACK && waiting() &&
             p.targetBoot == boot_ && p.request == request_) {
    state_ = State::ACTIVE;
    restart_ = true;
    log_(role_ == HIP ? "[Hip active; wrist acknowledged standby. Starting calibration]"
                     : "[Wrist active; hip acknowledged standby. Starting calibration]");
  }
}

void NodeSession::update(uint32_t now) {
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
  if (localActive()) log_("[Active | Other node acknowledged standby at activation]");
  else if (waiting()) log_("[Waiting for peer standby acknowledgement]");
  else if (state_ == State::FAILED) log_("[Activation failed | Inactive | Send ACTIVATE to retry]");
  else log_("[Standby | Send ACTIVATE to start]");
  // With no heartbeat, STATUS cannot claim that the peer is currently online.
}
