/**
 * @file application.h
 * @brief Bounded interface between application commands and the transport.
 *
 * The transport remains the sole owner of the connection. Complete received
 * frames cross into the application task through a fixed-size queue; logical
 * decisions return through a second fixed-size queue.
 */

#ifndef APPLICATION_H
#define APPLICATION_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "imu_source.h"
#include "protocol.h"
#include "physical_pipeline.h"

typedef enum {
    APPLICATION_RESPONSE_NONE = 0,
    APPLICATION_RESPONSE_DISCONNECT,
    APPLICATION_RESPONSE_TEST,
    APPLICATION_RESPONSE_ERROR,
    APPLICATION_RESPONSE_BENCHMARK_CONTROL,
    APPLICATION_RESPONSE_FILE,
    APPLICATION_RESPONSE_SYNC,
    APPLICATION_RESPONSE_CONTROL_ACK,
    APPLICATION_RESPONSE_ACTIVITY_TRIGGER,
} application_response_type_t;

typedef struct {
    application_response_type_t type;
    uint32_t session_id;
    imu_sample_t sample;
    uint16_t error_code;
    uint8_t offending_message_type;
    uint32_t control_sequence;
    protocol_benchmark_control_t benchmark_control;
    size_t file_frame_length;
    uint8_t file_frame[PROTOCOL_MAX_FRAME_SIZE];
    bool file_has_result;
    uint32_t file_sequence;
    protocol_file_result_t file_result;
    uint32_t sync_sequence;
    protocol_sync_response_t sync_response;
    uint32_t control_ack_response_sequence;
    protocol_control_ack_t control_ack;
    physical_trigger_event_t trigger_event;
} application_response_t;

/** Start the singleton application command task and its bounded queues. */
esp_err_t application_start(uint8_t local_node_id);

/**
 * Clear pending session-scoped work and enqueue a new session boundary.
 * Called only by the transport task after HELLO_ACK succeeds.
 */
bool application_begin_session(uint32_t session_id, uint32_t boot_id);

/** Copy one complete frame into the bounded application command queue. */
bool application_submit_frame(
    uint32_t session_id,
    const uint8_t *frame,
    size_t frame_length
);

/** Cancel pending session-scoped work after the connection is lost. */
void application_end_session(uint32_t session_id);

/** Poll one logical response without allowing the application to write. */
bool application_receive_response(
    application_response_t *response,
    TickType_t ticks_to_wait
);

#endif
