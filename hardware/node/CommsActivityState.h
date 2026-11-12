#pragma once

#include <stdint.h>

enum class UserActivity : uint8_t { IDLE, ACTIVE };
enum class ActivityResult {
  ACCEPTED, DUPLICATE, STALE_BOOT, STALE_SEQUENCE, SEQUENCE_CONFLICT,
  INVALID_SEQUENCE, INVALID_STATE
};

struct ActivityStatus {
  uint32_t boot = 0;
  uint32_t sequence = 0;
  UserActivity state = UserActivity::IDLE;
  bool received = false;
};

// User activity is independent of counting-board activation and rep sessions.
// Owned by the main loop. Only boot clears it; no timers or GPIO side effects.
class CommsActivityState {
 public:
  void begin(uint32_t boot);
  ActivityResult receive(uint32_t boot, uint32_t sequence, UserActivity state);
  ActivityStatus status() const { return status_; }

 private:
  ActivityStatus status_;
};

const char* userActivityName(UserActivity state);
const char* activityResultName(ActivityResult result);
