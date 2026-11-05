#pragma once

#include "NodeTypes.h"

namespace LinkTiming {
  constexpr uint32_t RETRY_MS = 500;
  constexpr uint8_t MAX_ATTEMPTS = 5;
  constexpr uint32_t REQUEST_TIMEOUT_MS = RETRY_MS * MAX_ATTEMPTS;
}

namespace LedTiming {
  constexpr uint32_t READY_STEP_MS = 100;
  constexpr uint32_t READY_BLINKS = 3;
  constexpr uint32_t READY_MS = READY_STEP_MS * 2 * READY_BLINKS;
  constexpr uint32_t REP_MS = 200;
}

enum class LedFeedback : uint8_t { CLEAR = 1, READY, REP };
enum class RepFeedback : uint8_t { CLEAR = 1, REP };
enum class PacketKind : uint8_t {
  DEACTIVATE = 1, STANDBY_ACK, SET_MODE, MODE_ACK, LED_FEEDBACK,
  REP_FEEDBACK, REP_FEEDBACK_ACK
};

namespace RepFeedbackTiming {
  constexpr uint8_t QUEUE_CAPACITY = 8;
  constexpr uint32_t RETRY_MS = 250;
  constexpr uint8_t MAX_ATTEMPTS = 5;
  constexpr uint32_t MAX_AGE_MS = RETRY_MS * MAX_ATTEMPTS;
}

// Selection, mode and feedback only. No counts, sensor samples or heartbeats.
struct LinkPacket {
  uint32_t magic;
  uint32_t boot;
  uint32_t targetBoot;
  uint32_t request;
  uint32_t sequence; // Separate LED/rep feedback order within activation; zero for commands.
  uint8_t version;
  PacketKind kind;
  uint8_t role;
  uint8_t mode; // Zero for activation; CountingMode, LedFeedback or RepFeedback by kind.
};
static_assert(sizeof(LinkPacket) == 24, "Keep control packets small and fixed-width");

// All methods run in loop(), never in the Wi-Fi callbacks.
class NodeSession {
 public:
  using Send = void (*)(const LinkPacket&);
  using Log = void (*)(const char*);
  using Feedback = void (*)(RepFeedback); // Main loop, HIP only; never a radio callback.
  NodeSession(Send send, Log log, Feedback feedback = nullptr);
  void begin(NodeRole role, uint32_t boot, CountingMode mode = PAUSE_MODE);
  void setMode(CountingMode mode, uint32_t now);
  // Called only for a changed exercise. Queue in standby, sync when active.
  void selectExerciseMode(CountingMode mode, uint32_t now);
  CountingMode selectedMode() const { return selectedMode_; }
  CountingMode mode() const { return mode_; }
  bool modePending() const { return modePending_; }
  void activate(uint32_t now);
  void standby(uint32_t now = 0);
  void receive(const LinkPacket& packet, uint32_t now);
  void update(uint32_t now);
  void status() const;
  bool consumeRestart();
  void showLedFeedback(LedFeedback feedback, uint32_t now);
  void clearLedFeedback(uint32_t now);
  bool feedbackLedOn(uint32_t now) const;
  // WRIST: called only for validated increases; clears on reset/deactivation.
  void showRepFeedback(uint32_t now);
  void clearRepFeedback(uint32_t now);
  bool localActive() const { return state_ == State::ACTIVE; }
  bool waiting() const { return state_ == State::WAITING; }
  TelemetryData output(const TelemetryData& local) const;

 private:
  enum class State { STANDBY, WAITING, ACTIVE, FAILED };
  NodeRole role_ = HIP;
  Send send_;
  Log log_;
  Feedback feedback_;
  State state_ = State::STANDBY;
  uint32_t boot_ = 0, request_ = 0;
  uint32_t requestedAt_ = 0, lastAttempt_ = 0;
  uint8_t attempts_ = 0;
  bool restart_ = false;
  CountingMode mode_ = PAUSE_MODE;
  // Local exercise preference/override survives the peer's synchronized mode.
  CountingMode selectedMode_ = PAUSE_MODE;
  uint32_t modeRequest_ = 0, modeRequestedAt_ = 0, modeLastAttempt_ = 0;
  uint32_t peerModeBoot_ = 0, peerModeRequest_ = 0;
  uint8_t modeAttempts_ = 0;
  bool modePending_ = false, peerModeAccepted_ = false;
  uint32_t activePeerBoot_ = 0, ledSendSequence_ = 0;
  uint32_t ledPeerBoot_ = 0, ledPeerRequest_ = 0, ledReceiveSequence_ = 0;
  uint32_t ledStartedAt_ = 0, ledDuration_ = 0;
  bool ledReadyPattern_ = false;
  struct PendingFeedback { uint32_t sequence, queuedAt; RepFeedback kind; };
  PendingFeedback repQueue_[RepFeedbackTiming::QUEUE_CAPACITY] = {};
  uint8_t repHead_ = 0, repCount_ = 0, repAttempts_ = 0;
  uint32_t repLastAttempt_ = 0, repSendSequence_ = 0, repReceiveSequence_ = 0;
  RepFeedback repReceivedKind_ = RepFeedback::CLEAR;
  void enqueueRepFeedback(RepFeedback kind, uint32_t now);
  void serviceRepFeedback(uint32_t now);
  void popRepFeedback();
  void receiveRepFeedback(const LinkPacket& packet, uint32_t now);
  void applyLedFeedback(LedFeedback feedback, uint32_t now);
  void applyMode(CountingMode mode);
  void requestMode(uint32_t now);
  void expireMode(uint32_t now);
  LinkPacket packet(PacketKind kind, uint32_t request, uint32_t targetBoot) const;
  void requestStandby(uint32_t now);
  bool expire(uint32_t now);
};
