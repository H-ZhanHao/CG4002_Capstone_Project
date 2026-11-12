#include "CommsRepCounter.h"

void CommsRepCounter::begin(uint32_t boot) {
  boot_ = boot ? boot : 1;
  session_ = 0;
  total_ = 0;
  received_ = false;
}

void CommsRepCounter::reset() {
  if (++session_ == 0) {
    // Rotate the ticket if the session counter wraps.
    if (++boot_ == 0) boot_ = 1;
    session_ = 1;
  }
  total_ = 0;
  received_ = false;
}

CommsRepResult CommsRepCounter::receive(uint32_t boot, uint32_t session, uint32_t total, bool active) {
  if (!active) return CommsRepResult::INACTIVE;
  if (!session_ || boot != boot_ || session != session_) return CommsRepResult::STALE_SESSION;
  if (total > INT32_MAX) return CommsRepResult::OUT_OF_RANGE;
  if (total < static_cast<uint32_t>(total_)) return CommsRepResult::DECREASING;
  if (received_ && total == static_cast<uint32_t>(total_)) return CommsRepResult::DUPLICATE;
  total_ = static_cast<int32_t>(total);
  received_ = true;
  return CommsRepResult::ACCEPTED;
}

const char* commsRepResultName(CommsRepResult result) {
  switch (result) {
    case CommsRepResult::ACCEPTED: return "ACCEPTED";
    case CommsRepResult::DUPLICATE: return "DUPLICATE";
    case CommsRepResult::INACTIVE: return "INACTIVE";
    case CommsRepResult::STALE_SESSION: return "STALE_SESSION";
    case CommsRepResult::DECREASING: return "DECREASING";
    case CommsRepResult::OUT_OF_RANGE: return "OUT_OF_RANGE";
  }
  return "UNKNOWN";
}
