#ifndef EXERCISE_BRIDGE_H
#define EXERCISE_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EXERCISE_BRIDGE_PUSHUP = 0,
    EXERCISE_BRIDGE_SITUP = 1,
    EXERCISE_BRIDGE_SQUAT = 2,
    EXERCISE_BRIDGE_LUNGE = 3,
    EXERCISE_BRIDGE_LEG_RAISE = 4,
    EXERCISE_BRIDGE_JUMPING_JACK = 5,
};

enum {
    EXERCISE_BRIDGE_PAUSE_MODE = 1,
    EXERCISE_BRIDGE_CONTINUOUS_MODE = 2,
};

/* Byte-for-byte storage for the hardware teammate's 24-byte LinkPacket.
 * Keeping it opaque prevents the C transport from depending on C++ enums. */
typedef struct {
    uint8_t bytes[24];
} exercise_bridge_peer_packet_t;

typedef struct {
    uint32_t repetitions;
    uint32_t peer_tx_drops;
    uint8_t exercise;
    uint8_t mode;
    int8_t gyro_axis;
    bool active;
    bool waiting_for_peer;
    bool led_on;
} exercise_bridge_status_t;

esp_err_t exercise_bridge_begin(uint8_t node_id, uint32_t boot_id);
void exercise_bridge_update(uint64_t timestamp_us,
    const float acceleration[3], const float gyro[3]);

bool exercise_bridge_activate(void);
void exercise_bridge_standby(void);
void exercise_bridge_reset(void);
bool exercise_bridge_select_exercise(uint8_t exercise);
bool exercise_bridge_select_mode(uint8_t mode);

/* These functions are called from the hardware/pipeline task, never from an
 * ESP-NOW callback. A radio callback must copy complete packets to a queue. */
bool exercise_bridge_receive_peer(const exercise_bridge_peer_packet_t *packet);
bool exercise_bridge_take_peer(exercise_bridge_peer_packet_t *packet);
void exercise_bridge_get_status(exercise_bridge_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
