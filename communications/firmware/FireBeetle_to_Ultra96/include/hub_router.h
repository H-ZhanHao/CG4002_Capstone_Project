/**
 * @file hub_router.h
 * @brief Routes locally produced and forwarded IMU samples upstream.
 */

#ifndef HUB_ROUTER_H
#define HUB_ROUTER_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Start the hub router and create its bounded producer-input queue.
 *
 * @param outbound_queue Queue consumed by the TCP transport.
 * @param input_queue    Receives the queue handle that producers submit to.
 *
 * @return ESP_OK on success or an ESP error code on failure.
 */
esp_err_t hub_router_start(
    QueueHandle_t outbound_queue,
    QueueHandle_t *input_queue
);

/** Return the cumulative number of oldest samples evicted upstream. */
uint32_t hub_router_get_dropped_count(void);

/** Return evictions attributed to the Node ID of the removed sample. */
uint32_t hub_router_get_dropped_count_for_node(uint8_t node_id);

#endif
