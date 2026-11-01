#include "FeedbackDiagnostic.h"
#include <string.h>

FeedbackDiagnostic::Command FeedbackDiagnostic::feed(char byte) {
  if (byte != '\r' && byte != '\n') {
    if (length_ < sizeof(line_) - 1 && byte >= 32 && byte <= 126)
      line_[length_++] = byte;
    else overflow_ = true;
    return Command::None;
  }
  line_[length_] = '\0';
  Command result = Command::Invalid;
  if (!overflow_) {
    if (!length_) result = Command::None;
    else if (!strcmp(line_, "feedback rep")) result = Command::Rep;
    else if (!strcmp(line_, "feedback ready")) result = Command::Ready;
    else if (!strcmp(line_, "feedback idle")) result = Command::Idle;
    else if (!strcmp(line_, "feedback fall-alarm")) result = Command::FallAlarm;
    else if (!strcmp(line_, "feedback status")) result = Command::Status;
  }
  length_ = 0;
  overflow_ = false;
  return result;
}
