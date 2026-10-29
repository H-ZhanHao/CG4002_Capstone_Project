/**
 * @file tcp_client.h
 * @brief Persistent TCP transport for encoded IMU samples.
 *
 * The TCP client owns the upstream socket, consumes samples from a FreeRTOS
 * queue, encodes them using the protocol module, sends complete frames, and
 * reconstructs downlink frames without blocking the sample send path.
 *
 * Connection failures are handled using bounded exponential backoff.
 */

#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "protocol.h"

typedef uint32_t (*tcp_client_overflow_count_fn_t)(uint8_t node_id);

/** All transport dependencies copied before the background task begins. */
typedef struct {
    QueueHandle_t sample_queue;
    const char *server_ip;
    uint16_t server_port;
    uint8_t hub_node_id;
    uint8_t firmware_major;
    uint8_t firmware_minor;
    uint8_t firmware_patch;
    uint16_t supported_sampling_rates;
    uint8_t assigned_node_mask;
    uint8_t active_node_mask;
    const uint8_t *device_token;
    /** Require authenticated TLS rather than development plain TCP. */
    bool use_tls;
    const char *tls_ca_certificate;
    const char *tls_server_identity;
    tcp_client_overflow_count_fn_t get_overflow_count;
    /** Queue items are protocol_physical_sample_t rather than legacy raw samples. */
    bool physical_samples;
} tcp_client_config_t;

/**
 * @brief Start the persistent TCP client task.
 *
 * @param config Queue, endpoint, hub identity, capabilities, and the
 *               non-sensitive Stage 3 development token. The function
 *               copies all required values before returning.
 *
 * @return ESP_OK on success or an ESP error code on failure.
 */
esp_err_t tcp_client_start(const tcp_client_config_t *config);

#endif
