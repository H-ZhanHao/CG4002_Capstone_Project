/**
 * @file espnow_link.h
 * @brief Encrypted unicast ESP-NOW transport between Leaf 1 and Hub 3.
 */

#ifndef ESPNOW_LINK_H
#define ESPNOW_LINK_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Role-neutral encrypted-peer identity and the role-owned sample queue. */
typedef struct {
    uint8_t local_node_id;
    uint8_t peer_node_id;
    uint8_t peer_mac[6];
    uint8_t channel;
    const uint8_t *pmk;
    const uint8_t *lmk;
    QueueHandle_t sample_queue;
} espnow_link_config_t;

/** Start Leaf transmission from sample_queue to the configured hub. */
esp_err_t espnow_link_start_leaf(const espnow_link_config_t *config);

/** Start Hub reception and route validated samples to sample_queue. */
esp_err_t espnow_link_start_hub(const espnow_link_config_t *config);

uint32_t espnow_link_get_send_failure_count(void);
/** Return callback-to-task queue evictions on the hub. */
uint32_t espnow_link_get_receive_drop_count(void);
/** Return malformed, unauthorized, or undecodable received datagrams. */
uint32_t espnow_link_get_invalid_frame_count(void);

#endif
