/**
 * @file imu_source.h
 * @brief Common representation of one captured IMU sample.
 *
 * This header defines the sensor-data structure shared by the mock IMU
 * producer, the future physical IMU driver, the protocol encoder, and the
 * transport pipeline.
 *
 * Sequence numbers and timestamps describe the original capture event.
 * They must be assigned by the sample producer and preserved while the
 * sample passes through queues, hubs, and network transports.
 *
 * Replacing the mock source with a physical IMU should not require changes
 * to the protocol encoder or TCP transport.
 */

#ifndef IMU_SOURCE_H
#define IMU_SOURCE_H

#include <stdint.h>

typedef struct {
    uint8_t node_id;       /**< Original ESP32/IMU source, from 1 to 6. */
    uint8_t flags;         /**< Packet-level condition flags. */
    uint32_t sequence;     /**< Per-node sequence assigned at capture. */
    uint64_t timestamp_us; /**< Monotonic capture time in microseconds. */

    int16_t ax;            /**< Raw X-axis acceleration. */
    int16_t ay;            /**< Raw Y-axis acceleration. */
    int16_t az;            /**< Raw Z-axis acceleration. */
    int16_t gx;            /**< Raw X-axis angular velocity. */
    int16_t gy;            /**< Raw Y-axis angular velocity. */
    int16_t gz;            /**< Raw Z-axis angular velocity. */

    uint16_t status;       /**< Sensor and sample status bits. */
} imu_sample_t;

#endif
