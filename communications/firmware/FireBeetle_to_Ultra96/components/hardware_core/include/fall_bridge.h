#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Pipeline task only. Reuses canonical hardware detector without new IMU reads.
void fall_bridge_begin(uint8_t node);
bool fall_bridge_update(const float acceleration[3], uint64_t timestamp_us, bool valid);
#ifdef __cplusplus
}
#endif
