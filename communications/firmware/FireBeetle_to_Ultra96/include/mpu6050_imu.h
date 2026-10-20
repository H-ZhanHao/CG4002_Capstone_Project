/**
 * @file mpu6050_imu.h
 * @brief 100 Hz MPU6050 producer for the FireBeetle ESP32.
 */

#ifndef MPU6050_IMU_H
#define MPU6050_IMU_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t mpu6050_imu_start(QueueHandle_t sample_queue, uint8_t node_id);
esp_err_t mpu6050_imu_set_paused(uint8_t node_id, bool paused);
esp_err_t mpu6050_imu_schedule_start(uint8_t node_id, int64_t local_start_us);
esp_err_t mpu6050_imu_stop(uint8_t node_id);
uint32_t mpu6050_imu_get_dropped_count(void);

#endif
