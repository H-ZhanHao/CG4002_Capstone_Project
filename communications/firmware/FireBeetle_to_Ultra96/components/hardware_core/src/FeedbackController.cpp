#include "FeedbackController.h"

void FeedbackController::begin(uint8_t node) {
  hip_ = node == 1;
  ledOn_ = false;
  connectionFeedback_ = serverConnected_ = confirming_ = false;
  now_ = connectionSince_ = 0;
  buzzer_.begin();
}

void FeedbackController::enableConnectionFeedback(uint32_t now) {
  connectionFeedback_ = true;
  serverConnected_ = confirming_ = false;
  connectionSince_ = now;
  now_ = now;
}

void FeedbackController::setServerConnected(bool connected, uint32_t now) {
  update(now);
  if (!connectionFeedback_ || connected == serverConnected_) return;
  serverConnected_ = connected;
  confirming_ = connected;
  connectionSince_ = now;
  if (!connected) ledOn_ = false; // Never restore stale streaming after reconnect.
}

bool FeedbackController::ledOn() const {
  if (!connectionFeedback_) return ledOn_;
  const uint32_t elapsed = now_ - connectionSince_;
  if (!serverConnected_) return (elapsed / CONNECT_HALF_PERIOD_MS) % 2 == 0;
  // Begin with a short OFF separator so all three pulses are distinct even
  // when HELLO_ACK arrives during the connecting pattern's ON phase.
  if (confirming_) return (elapsed / CONNECTED_HALF_PERIOD_MS) % 2 == 1;
  return ledOn_;
}

void FeedbackController::resetSession() {
  ledOn_ = false;
  if (connectionFeedback_) {
    // The old accepted server session no longer owns the LED. A subsequent
    // BEGIN must confirm its own handshake even if teardown was coalesced.
    serverConnected_ = confirming_ = false;
    connectionSince_ = now_;
  }
  buzzer_.cancelReps();
}

bool FeedbackController::trigger(Action action, uint32_t now) {
  update(now);
  switch (action) {
    case Action::Rep:
      if (!hip_) return false;
      // Canonical hardware queue: 100 ms pulses/gaps; falls suppress reps.
      buzzer_.rep(now);
      break;
    case Action::Active:
      ledOn_ = true;
      break;
    case Action::Idle:
      ledOn_ = false;
      break;
    case Action::FallAlarm:
      if (!hip_) return false;
      buzzer_.fall(now);
      break;
    default:
      return false;
  }
  return true;
}

void FeedbackController::update(uint32_t now) {
  now_ = now;
  if (confirming_ && now - connectionSince_ >= CONNECTED_PATTERN_MS)
    confirming_ = false;
  buzzer_.update(now);
}

const char *FeedbackController::ledPattern() const {
  if (connectionFeedback_ && !serverConnected_) return "connecting";
  if (confirming_) return "connected-confirmation";
  return ledOn_ ? "active" : "idle";
}

const char *FeedbackController::buzzerPattern() const {
  return buzzer_.fallActive() ? "fall-alarm" : buzzerOn() ? "rep" : "off";
}
