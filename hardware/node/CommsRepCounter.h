#pragma once

#include <stdint.h>

enum class CommsRepResult { ACCEPTED, DUPLICATE, INACTIVE, STALE_SESSION, DECREASING, OUT_OF_RANGE };

// Main-loop state only. Comms sends cumulative totals for the HW-issued ticket.
class CommsRepCounter {
 public:
  void begin(uint32_t boot);
  void reset();
  CommsRepResult receive(uint32_t boot, uint32_t session, uint32_t total, bool active);
  uint32_t boot() const { return boot_; }
  uint32_t session() const { return session_; }
  int32_t total() const { return total_; }
  bool received() const { return received_; }

 private:
  uint32_t boot_ = 0, session_ = 0;
  int32_t total_ = 0;
  bool received_ = false;
};

const char* commsRepResultName(CommsRepResult result);
