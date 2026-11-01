#pragma once

#include <stdint.h>

// Pipeline-owned, allocation-free state machine. All timestamps are uint32 ms;
// unsigned subtraction keeps the short patterns correct across timer rollover.
class FeedbackController {
 public:
  enum class Action { Rep, Ready, Idle, FallAlarm };
  void begin(uint8_t node);
  bool trigger(Action action, uint32_t now);
  void update(uint32_t now);
  bool ledOn() const { return ledOn_; }
  bool buzzerOn() const { return buzzerOn_; }
  bool hasBuzzer() const { return hip_; }
  const char *ledPattern() const;
  const char *buzzerPattern() const;
 private:
  enum class Led { Off, Rep, Ready };
  enum class Buzzer { Off, Idle, Fall };
  Led led_ = Led::Off;
  Buzzer buzzer_ = Buzzer::Off;
  uint32_t ledStart_ = 0, buzzerStart_ = 0;
  bool hip_ = false, ledOn_ = false, buzzerOn_ = false;
};
