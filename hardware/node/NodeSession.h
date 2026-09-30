#pragma once

#include "NodeTypes.h"

namespace LinkTiming {
  constexpr uint32_t RETRY_MS = 500;
  constexpr uint8_t MAX_ATTEMPTS = 5;
  constexpr uint32_t REQUEST_TIMEOUT_MS = RETRY_MS * MAX_ATTEMPTS;
}

enum class PacketKind : uint8_t { DEACTIVATE = 1, STANDBY_ACK };

// Only selection commands travel over ESP-NOW. No sensor data or heartbeats.
struct LinkPacket {
  uint32_t magic;
  uint32_t boot;
  uint32_t targetBoot;
  uint32_t request;
  uint8_t version;
  PacketKind kind;
  uint8_t role;
  uint8_t reserved;
};
static_assert(sizeof(LinkPacket) == 20, "Keep control packets small and fixed-width");

// All methods run in loop(), never in the Wi-Fi callbacks.
class NodeSession {
 public:
  using Send = void (*)(const LinkPacket&);
  using Log = void (*)(const char*);
  NodeSession(Send send, Log log);
  void begin(NodeRole role, uint32_t boot);
  void activate(uint32_t now);
  void standby();
  void receive(const LinkPacket& packet, uint32_t now);
  void update(uint32_t now);
  void status() const;
  bool consumeRestart();
  bool localActive() const { return state_ == State::ACTIVE; }
  bool waiting() const { return state_ == State::WAITING; }
  TelemetryData output(const TelemetryData& local) const;

 private:
  enum class State { STANDBY, WAITING, ACTIVE, FAILED };
  NodeRole role_ = HIP;
  Send send_;
  Log log_;
  State state_ = State::STANDBY;
  uint32_t boot_ = 0, request_ = 0;
  uint32_t requestedAt_ = 0, lastAttempt_ = 0;
  uint8_t attempts_ = 0;
  bool restart_ = false;
  LinkPacket packet(PacketKind kind, uint32_t request, uint32_t targetBoot) const;
  void requestStandby(uint32_t now);
  bool expire(uint32_t now);
};
