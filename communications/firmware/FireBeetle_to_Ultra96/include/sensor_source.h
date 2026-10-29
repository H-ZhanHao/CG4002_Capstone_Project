/** Select the physical source by default while retaining an explicit mock build. */
#ifndef SENSOR_SOURCE_H
#define SENSOR_SOURCE_H

#include "app_config.h"

#if APP_USE_MOCK_IMU
#include "mock_imu.h"
#define sensor_source_start             mock_imu_start
#define sensor_source_set_paused        mock_imu_set_paused
#define sensor_source_schedule_start    mock_imu_schedule_start
#define sensor_source_stop              mock_imu_stop
#define sensor_source_get_dropped_count mock_imu_get_dropped_count
#else
#include "mpu6050_imu.h"
#define sensor_source_start             mpu6050_imu_start
#define sensor_source_set_paused        mpu6050_imu_set_paused
#define sensor_source_schedule_start    mpu6050_imu_schedule_start
#define sensor_source_stop              mpu6050_imu_stop
#define sensor_source_get_dropped_count mpu6050_imu_get_dropped_count
#endif

#endif
