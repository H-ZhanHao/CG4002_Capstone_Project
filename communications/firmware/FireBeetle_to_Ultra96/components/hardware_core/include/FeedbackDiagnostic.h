#pragma once
#include <stddef.h>

// No dynamic allocation, and an oversized line is discarded in its entirety.
class FeedbackDiagnostic {
 public:
  enum class Command { None, Rep, Ready, Idle, FallAlarm, FallEvent, Status, Invalid };
  Command feed(char byte);
 private:
  char line_[48] = {};
  size_t length_ = 0;
  bool overflow_ = false;
};
