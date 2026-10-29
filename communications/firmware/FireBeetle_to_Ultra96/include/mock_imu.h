/**
 * @file mock_imu.h
 * @brief Deterministic 50 Hz mock IMU sources for development.
 */

#ifndef MOCK_IMU_H
#define MOCK_IMU_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t mock_imu_start(
    QueueHandle_t sample_queue,
    uint8_t node_id
);

/** Pause or resume one mock source without stopping its FreeRTOS task. */
esp_err_t mock_imu_set_paused(uint8_t node_id, bool paused);

/** Arm a stopped source for an absolute local esp_timer timestamp. */
esp_err_t mock_imu_schedule_start(uint8_t node_id, int64_t local_start_us);

/** Idempotently stop a source, cancel an armed start, and discard queued data. */
esp_err_t mock_imu_stop(uint8_t node_id);

/** Return total producer-input drops across all mock sources. */
uint32_t mock_imu_get_dropped_count(void);

#endif
