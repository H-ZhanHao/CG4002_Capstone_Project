#include "CommsActivityState.h"

void CommsActivityState::begin(uint32_t boot) {
  status_ = ActivityStatus{};
  status_.boot = boot ? boot : 1;
}

ActivityResult CommsActivityState::receive(uint32_t boot, uint32_t sequence, UserActivity state) {
  if (!status_.boot || boot != status_.boot) return ActivityResult::STALE_BOOT;
  if (!sequence) return ActivityResult::INVALID_SEQUENCE;
  if (state != UserActivity::IDLE && state != UserActivity::ACTIVE)
    return ActivityResult::INVALID_STATE;
  if (sequence < status_.sequence) return ActivityResult::STALE_SEQUENCE;
  if (sequence == status_.sequence)
    return state == status_.state ? ActivityResult::DUPLICATE : ActivityResult::SEQUENCE_CONFLICT;
  status_.sequence = sequence;
  status_.state = state;
  status_.received = true;
  return ActivityResult::ACCEPTED;
}

const char* userActivityName(UserActivity state) {
  switch (state) {
    case UserActivity::IDLE: return "IDLE";
    case UserActivity::ACTIVE: return "ACTIVE";
  }
  return "UNKNOWN";
}

const char* activityResultName(ActivityResult result) {
  switch (result) {
    case ActivityResult::ACCEPTED: return "ACCEPTED";
    case ActivityResult::DUPLICATE: return "DUPLICATE";
    case ActivityResult::STALE_BOOT: return "STALE_BOOT";
    case ActivityResult::STALE_SEQUENCE: return "STALE_SEQUENCE";
    case ActivityResult::SEQUENCE_CONFLICT: return "SEQUENCE_CONFLICT";
    case ActivityResult::INVALID_SEQUENCE: return "INVALID_SEQUENCE";
    case ActivityResult::INVALID_STATE: return "INVALID_STATE";
  }
  return "UNKNOWN";
}
