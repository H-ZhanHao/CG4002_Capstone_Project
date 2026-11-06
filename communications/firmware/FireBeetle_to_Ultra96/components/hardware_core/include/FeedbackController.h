#pragma once

#include <stdint.h>
#include "BuzzerFeedback.h"

// Pipeline-owned, allocation-free state machine. All timestamps are uint32 ms;
// unsigned subtraction keeps the short patterns correct across timer rollover.
class FeedbackController {
 public:
  enum class Action { Rep, Active, Idle, FallAlarm };
  void begin(uint8_t node);
  void resetSession(); // Clear connection/activity/reps; preserve local fall alarm.
  // Optional local connection indication, independent of server activity commands.
  static constexpr uint32_t CONNECT_HALF_PERIOD_MS = 500;
  static constexpr uint32_t CONNECTED_HALF_PERIOD_MS = 100;
  static constexpr uint32_t CONNECTED_BLINK_COUNT = 3;
  static constexpr uint32_t CONNECTED_PATTERN_MS =
      (2 * CONNECTED_BLINK_COUNT + 1) * CONNECTED_HALF_PERIOD_MS;
  void enableConnectionFeedback(uint32_t now);
  void setServerConnected(bool connected, uint32_t now);
  bool trigger(Action action, uint32_t now);
  void update(uint32_t now);
  bool ledOn() const;
  bool buzzerOn() const { return hip_ && buzzer_.on(); }
  bool hasBuzzer() const { return hip_; }
  const char *ledPattern() const;
  const char *buzzerPattern() const;
 private:
  BuzzerFeedback buzzer_;
  bool hip_ = false, ledOn_ = false;
  bool connectionFeedback_ = false, serverConnected_ = false, confirming_ = false;
  uint32_t now_ = 0, connectionSince_ = 0;
};
