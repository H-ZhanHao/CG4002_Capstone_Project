#pragma once

#include <stdint.h>
#include "BuzzerFeedback.h"

// Pipeline-owned, allocation-free state machine. All timestamps are uint32 ms;
// unsigned subtraction keeps the short patterns correct across timer rollover.
class FeedbackController {
 public:
  enum class Action { Rep, Active, Idle, FallAlarm };
  void begin(uint8_t node);
  void resetSession(); // Clear activity/reps, preserving any local fall alarm.
  bool trigger(Action action, uint32_t now);
  void update(uint32_t now);
  bool ledOn() const { return ledOn_; }
  bool buzzerOn() const { return hip_ && buzzer_.on(); }
  bool hasBuzzer() const { return hip_; }
  const char *ledPattern() const;
  const char *buzzerPattern() const;
 private:
  BuzzerFeedback buzzer_;
  bool hip_ = false, ledOn_ = false;
};
