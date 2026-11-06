#include "FeedbackController.h"

void FeedbackController::begin(uint8_t node) {
  hip_ = node == 1;
  ledOn_ = false;
  buzzer_.begin();
}

void FeedbackController::resetSession() {
  ledOn_ = false;
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
  buzzer_.update(now);
}

const char *FeedbackController::ledPattern() const {
  return ledOn_ ? "active" : "idle";
}

const char *FeedbackController::buzzerPattern() const {
  return buzzer_.fallActive() ? "fall-alarm" : buzzerOn() ? "rep" : "off";
}
