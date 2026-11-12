#include "BuzzerFeedback.h"
#include "ExerciseConfig.h"

void BuzzerFeedback::begin() {
  state_ = State::OFF;
  started_ = pending_ = 0;
}

void BuzzerFeedback::rep(uint32_t now) {
  update(now);
  if (state_ == State::FALL) return;
  if (state_ == State::OFF) {
    state_ = State::REP;
    started_ = now;
  } else if (pending_ < UINT32_MAX) {
    ++pending_;
  }
}

void BuzzerFeedback::fall(uint32_t now) {
  pending_ = 0;
  state_ = State::FALL;
  started_ = now;
}

void BuzzerFeedback::cancelReps() {
  pending_ = 0;
  if (state_ != State::FALL) state_ = State::OFF;
}

void BuzzerFeedback::update(uint32_t now) {
  const uint32_t elapsed = now - started_; // Safe across millis() rollover.
  if (state_ == State::FALL && elapsed >= FallConfig::BUZZER_DURATION_MS) {
    state_ = State::OFF;
  } else if (state_ == State::REP && elapsed >= FeedbackConfig::REP_BEEP_MS) {
    state_ = State::GAP;
    started_ = now;
  } else if (state_ == State::GAP && elapsed >= FeedbackConfig::REP_GAP_MS) {
    state_ = pending_ ? State::REP : State::OFF;
    if (pending_) --pending_;
    started_ = now;
  }
}
