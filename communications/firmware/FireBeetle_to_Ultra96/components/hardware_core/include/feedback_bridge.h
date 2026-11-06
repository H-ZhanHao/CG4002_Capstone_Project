#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
// Startup only, then service exclusively from the physical pipeline task.
typedef enum {
  FEEDBACK_REP_BUZZ, FEEDBACK_ACTIVITY_ACTIVE, FEEDBACK_ACTIVITY_IDLE, FEEDBACK_FALL_ALARM
} feedback_action_t;
esp_err_t feedback_bridge_begin(uint8_t node_id);
void feedback_bridge_reset_session(void);
// Local output requests only. FALL_ALARM does not create a detected-fall event.
// Call only on the pipeline task; no GPIO or state access from other tasks.
bool feedback_bridge_trigger(feedback_action_t action, uint32_t now_ms);
void feedback_bridge_service(uint32_t now_ms);
// Optional explicitly synthetic fall event diagnostic, invoked on pipeline task.
void feedback_bridge_set_fall_injector(bool (*injector)(void));
#ifdef __cplusplus
}
#endif
