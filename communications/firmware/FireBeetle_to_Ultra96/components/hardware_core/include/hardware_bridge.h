#ifndef HARDWARE_BRIDGE_H
#define HARDWARE_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t timestamp_us;
    float acceleration[3];
    float gyro[3];
} hardware_bridge_sample_t;

typedef struct {
    uint64_t peak_timestamp_us;
    uint64_t detected_timestamp_us;
    float acceleration_mps2;
    float gyro_rad_s;
} hardware_bridge_peak_t;

esp_err_t hardware_bridge_begin(void);
bool hardware_bridge_capture(uint32_t sample_index,
    hardware_bridge_sample_t *sample, hardware_bridge_peak_t *peak);
void hardware_bridge_reset_peak_detector(void);

#ifdef __cplusplus
}
#endif

#endif

