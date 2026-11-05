#pragma once

#include <stdint.h>

// Main-loop timing only; the caller owns GPIO and restricts output to HIP.
class BuzzerFeedback {
 public:
  void begin();
  void rep(uint32_t now);
  void fall(uint32_t now);
  void cancelReps();
  void update(uint32_t now);
  bool on() const { return state_ == State::REP || state_ == State::FALL; }
  bool fallActive() const { return state_ == State::FALL; }

 private:
  enum class State { OFF, REP, GAP, FALL };
  State state_ = State::OFF;
  uint32_t started_ = 0;
  uint32_t pending_ = 0;
};
