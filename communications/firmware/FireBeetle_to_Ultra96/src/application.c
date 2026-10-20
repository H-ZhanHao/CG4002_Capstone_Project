/** @file application.c @brief Session-scoped application command processing. */
#include "application.h"

#include <limits.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "protocol.h"
#include "sensor_source.h"
#include "psa/crypto.h"

#define APPLICATION_TASK_STACK_SIZE 4096
#define APPLICATION_TASK_PRIORITY 4
#define APPLICATION_COMMAND_QUEUE_LENGTH 4
#define APPLICATION_RESPONSE_QUEUE_LENGTH 4
#define TEST_REQUEST_CACHE_SIZE 4
#define FILE_TRANSFER_MAX_SIZE (1024U * 1024U)
#define FILE_TRANSFER_TIMEOUT_MS 10000

typedef enum {
    APPLICATION_COMMAND_BEGIN_SESSION,
    APPLICATION_COMMAND_FRAME,
    APPLICATION_COMMAND_END_SESSION,
} application_command_type_t;

typedef struct {
    application_command_type_t type;
    uint32_t session_id;
    size_t frame_length;
    uint64_t received_at_us;
    uint8_t frame[PROTOCOL_MAX_FRAME_SIZE];
} application_command_t;

typedef struct {
    bool valid;
    uint32_t sequence;
    uint8_t request_payload[PROTOCOL_TEST_PAYLOAD_SIZE];
    imu_sample_t response;
} test_cache_entry_t;

typedef struct {
    bool active;
    uint32_t file_id;
    uint32_t total_size;
    uint32_t next_offset;
    uint16_t chunk_data_size;
    uint8_t expected_sha256[PROTOCOL_SHA256_SIZE];
    int64_t last_activity_us;
    psa_hash_operation_t sha256;
} file_transfer_state_t;

typedef struct {
    bool valid;
    uint32_t sequence;
    size_t payload_length;
    uint8_t payload[PROTOCOL_START_PAYLOAD_SIZE];
    protocol_control_ack_t ack;
} control_cache_entry_t;

typedef struct {
    bool capture_requested;
    control_cache_entry_t start;
    control_cache_entry_t stop;
} control_state_t;

static const char *TAG = "application_command";
static QueueHandle_t command_queue;
static QueueHandle_t response_queue;
static TaskHandle_t application_task_handle;
static uint8_t configured_local_node_id;
/* Singleton application task owns this maximum-size queue item. */
static application_response_t application_response_workspace;

/* Return one logical result to the transport without giving this task socket ownership. */
static bool enqueue_response(const application_response_t *response)
{
    if (xQueueSend(response_queue, response, pdMS_TO_TICKS(250)) != pdPASS) {
        ESP_LOGE(TAG, "Response queue full; response dropped");
        return false;
    }
    return true;
}

/* Ask the transport to discard a Session after an unrecoverable command error. */
static void request_disconnect(uint32_t session_id)
{
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_DISCONNECT;
    application_response_workspace.session_id = session_id;
    (void)enqueue_response(&application_response_workspace);
}

/* Convert a validation failure into the fields needed for a protocol ERROR frame. */
static void enqueue_error(
    uint32_t session_id, uint32_t sequence,
    uint16_t error_code, uint8_t offending_message_type)
{
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_ERROR;
    application_response_workspace.session_id = session_id;
    application_response_workspace.error_code = error_code;
    application_response_workspace.offending_message_type = offending_message_type;
    application_response_workspace.sample.sequence = sequence;
    (void)enqueue_response(&application_response_workspace);
}

/* Return a transformed TEST_RESPONSE while preserving request correlation fields. */
static void enqueue_test_response(uint32_t session_id, const imu_sample_t *sample)
{
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_TEST;
    application_response_workspace.session_id = session_id;
    application_response_workspace.sample = *sample;
    (void)enqueue_response(&application_response_workspace);
}

/* Abort any active SHA-256 operation and return file state to its empty boundary. */
static void reset_file_transfer(file_transfer_state_t *state)
{
    if (state->active) (void)psa_hash_abort(&state->sha256);
    memset(state, 0, sizeof(*state));
}

/* Calculate the standard CRC-32 carried by each independently checked chunk. */
static uint32_t file_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}

/* Finalize the whole-file digest only after every ordered chunk is accepted. */
static bool finish_file_sha256(
    file_transfer_state_t *state, uint8_t digest[PROTOCOL_SHA256_SIZE])
{
    size_t digest_length = 0;
    return psa_hash_finish(&state->sha256, digest, PROTOCOL_SHA256_SIZE,
        &digest_length) == PSA_SUCCESS && digest_length == PROTOCOL_SHA256_SIZE;
}

/* Package file progress or its final verification result for the transport task. */
static void enqueue_file_response(
    const application_command_t *command, const protocol_header_t *header,
    uint16_t result_code, const uint8_t calculated_sha256[PROTOCOL_SHA256_SIZE],
    uint32_t file_id, uint32_t bytes_received, bool forward)
{
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_FILE;
    application_response_workspace.session_id = command->session_id;
    application_response_workspace.file_frame_length =
        forward ? command->frame_length : 0;
    application_response_workspace.file_has_result = result_code != UINT16_MAX;
    application_response_workspace.file_sequence = header->sequence;
    application_response_workspace.file_result.file_id = file_id;
    application_response_workspace.file_result.result_code = result_code;
    application_response_workspace.file_result.bytes_received = bytes_received;
    if (forward) memcpy(application_response_workspace.file_frame,
        command->frame, command->frame_length);
    if (calculated_sha256 != NULL)
        memcpy(application_response_workspace.file_result.calculated_sha256,
            calculated_sha256,
            PROTOCOL_SHA256_SIZE);
    (void)enqueue_response(&application_response_workspace);
}

/* Apply one BEGIN/CHUNK/END frame to the Session-scoped file state machine. */
static void process_file_frame(
    const application_command_t *command, const protocol_header_t *header,
    file_transfer_state_t *state)
{
    const uint8_t *payload = command->frame + PROTOCOL_HEADER_SIZE;
    if (header->node_id != 0 || header->flags != 0) {
        enqueue_file_response(command, header, PROTOCOL_FILE_RESULT_INVALID_STATE,
            NULL, state->active ? state->file_id : 0,
            state->next_offset, false);
        reset_file_transfer(state);
        return;
    }

    if (header->message_type == PROTOCOL_MESSAGE_TYPE_FILE_BEGIN) {
        protocol_file_begin_t begin = {0};
        if (state->active ||
            !protocol_decode_file_begin_payload(payload, header->payload_length, &begin) ||
            begin.file_id == 0 || begin.total_size == 0 ||
            begin.total_size > FILE_TRANSFER_MAX_SIZE || begin.chunk_data_size == 0 ||
            begin.chunk_data_size > PROTOCOL_FILE_CHUNK_MAX_DATA_SIZE) {
            enqueue_file_response(command, header, PROTOCOL_FILE_RESULT_INVALID_STATE,
                NULL, begin.file_id, 0, false);
            if (state->active) reset_file_transfer(state);
            return;
        }
        memset(state, 0, sizeof(*state));
        state->active = true;
        state->file_id = begin.file_id;
        state->total_size = begin.total_size;
        state->chunk_data_size = begin.chunk_data_size;
        memcpy(state->expected_sha256, begin.expected_sha256, PROTOCOL_SHA256_SIZE);
        state->last_activity_us = esp_timer_get_time();
        state->sha256 = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
        if (psa_crypto_init() != PSA_SUCCESS ||
            psa_hash_setup(&state->sha256, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            enqueue_file_response(command, header, PROTOCOL_FILE_RESULT_INVALID_STATE,
                NULL, begin.file_id, 0, false);
            reset_file_transfer(state);
            return;
        }
        ESP_LOGI(TAG, "File transfer started: id=%lu size=%lu chunk=%u",
            (unsigned long)begin.file_id, (unsigned long)begin.total_size,
            begin.chunk_data_size);
        enqueue_file_response(command, header, UINT16_MAX, NULL,
            begin.file_id, 0, true);
        return;
    }

    if (header->message_type == PROTOCOL_MESSAGE_TYPE_FILE_CHUNK) {
        protocol_file_chunk_t chunk = {0};
        uint16_t result = PROTOCOL_FILE_RESULT_INVALID_STATE;
        if (protocol_decode_file_chunk_payload(payload, header->payload_length, &chunk) &&
            state->active && chunk.file_id == state->file_id) {
            result = PROTOCOL_FILE_RESULT_UNEXPECTED_OFFSET;
            bool length_fits = chunk.offset <= state->total_size &&
                chunk.data_length <= state->total_size - chunk.offset;
            bool final_chunk = length_fits &&
                chunk.offset + chunk.data_length == state->total_size;
            if (chunk.offset == state->next_offset && chunk.data_length > 0 &&
                length_fits &&
                (chunk.data_length == state->chunk_data_size || final_chunk)) {
                result = PROTOCOL_FILE_RESULT_CRC_MISMATCH;
                if (file_crc32(chunk.data, chunk.data_length) == chunk.crc32 &&
                    psa_hash_update(&state->sha256, chunk.data,
                        chunk.data_length) == PSA_SUCCESS) {
                    state->next_offset += chunk.data_length;
                    state->last_activity_us = esp_timer_get_time();
                    enqueue_file_response(command, header, UINT16_MAX, NULL,
                        state->file_id, state->next_offset, true);
                    return;
                }
            }
        }
        enqueue_file_response(command, header, result, NULL,
            state->active ? state->file_id : chunk.file_id,
            state->next_offset, false);
        ESP_LOGE(TAG, "File transfer rejected: result=%u offset=%lu", result,
            (unsigned long)state->next_offset);
        reset_file_transfer(state);
        return;
    }

    protocol_file_end_t end;
    uint8_t digest[PROTOCOL_SHA256_SIZE] = {0};
    uint16_t result = PROTOCOL_FILE_RESULT_INVALID_STATE;
    uint32_t file_id = state->active ? state->file_id : 0;
    uint32_t bytes_received = state->next_offset;
    if (protocol_decode_file_end_payload(payload, header->payload_length, &end) &&
        state->active && end.file_id == state->file_id) {
        file_id = end.file_id;
        if (end.total_size != state->total_size ||
            state->next_offset != state->total_size) {
            result = PROTOCOL_FILE_RESULT_SIZE_MISMATCH;
        } else if (memcmp(end.expected_sha256, state->expected_sha256,
                       PROTOCOL_SHA256_SIZE) != 0 ||
                   !finish_file_sha256(state, digest) ||
                   memcmp(digest, state->expected_sha256,
                       PROTOCOL_SHA256_SIZE) != 0) {
            result = PROTOCOL_FILE_RESULT_SHA256_MISMATCH;
        } else {
            result = PROTOCOL_FILE_RESULT_SUCCESS;
        }
    }
    enqueue_file_response(command, header, result, digest, file_id,
        bytes_received, result == PROTOCOL_FILE_RESULT_SUCCESS);
    ESP_LOGI(TAG, "File transfer finished: id=%lu bytes=%lu result=%u",
        (unsigned long)file_id, (unsigned long)bytes_received, result);
    reset_file_transfer(state);
}

/* Validate benchmark parameters and pass the requested mode to the transport. */
static void process_benchmark_control(
    const application_command_t *command, const protocol_header_t *header)
{
    protocol_benchmark_control_t control;
    if (header->node_id != 0 || header->flags != 0 ||
        !protocol_decode_benchmark_control_payload(
            command->frame + PROTOCOL_HEADER_SIZE,
            header->payload_length, &control) ||
        (control.action == PROTOCOL_BENCHMARK_ACTION_START &&
            (control.batch_frames == 0 || control.duration_ms == 0))) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_INVALID_FRAME,
            PROTOCOL_MESSAGE_TYPE_BENCHMARK_CONTROL);
        return;
    }
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_BENCHMARK_CONTROL;
    application_response_workspace.session_id = command->session_id;
    application_response_workspace.control_sequence = header->sequence;
    application_response_workspace.benchmark_control = control;
    (void)enqueue_response(&application_response_workspace);
}

static void process_sync_request(
    const application_command_t *command, const protocol_header_t *header)
{
    uint64_t coordinator_send_us;
    if (header->node_id != 0 || header->flags != 0 ||
        !protocol_decode_sync_request_payload(
            command->frame + PROTOCOL_HEADER_SIZE,
            header->payload_length, &coordinator_send_us) ||
        coordinator_send_us != header->timestamp_us) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_INVALID_FRAME, PROTOCOL_MESSAGE_TYPE_SYNC_REQ);
        return;
    }
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_SYNC;
    application_response_workspace.session_id = command->session_id;
    application_response_workspace.sync_sequence = header->sequence;
    application_response_workspace.sync_response.coordinator_send_us =
        coordinator_send_us;
    application_response_workspace.sync_response.node_receive_us =
        command->received_at_us;
    application_response_workspace.sync_response.node_send_us =
        (uint64_t)esp_timer_get_time();
    (void)enqueue_response(&application_response_workspace);
}

static void enqueue_control_ack(
    uint32_t session_id, uint32_t response_sequence,
    const protocol_control_ack_t *ack)
{
    memset(&application_response_workspace, 0, sizeof(application_response_workspace));
    application_response_workspace.type = APPLICATION_RESPONSE_CONTROL_ACK;
    application_response_workspace.session_id = session_id;
    application_response_workspace.control_ack_response_sequence =
        response_sequence;
    application_response_workspace.control_ack = *ack;
    (void)enqueue_response(&application_response_workspace);
}

static bool replay_control_if_cached(
    const application_command_t *command, const protocol_header_t *header,
    control_cache_entry_t *cache)
{
    if (!cache->valid || cache->sequence != header->sequence) return false;
    if (cache->payload_length != header->payload_length ||
        memcmp(cache->payload, command->frame + PROTOCOL_HEADER_SIZE,
            header->payload_length) != 0) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_SEQUENCE_REUSE, header->message_type);
        return true;
    }
    enqueue_control_ack(command->session_id, header->sequence, &cache->ack);
    return true;
}

static void cache_control_result(
    const application_command_t *command, const protocol_header_t *header,
    control_cache_entry_t *cache, const protocol_control_ack_t *ack)
{
    memset(cache, 0, sizeof(*cache));
    cache->valid = true;
    cache->sequence = header->sequence;
    cache->payload_length = header->payload_length;
    if (header->payload_length > 0) {
        memcpy(cache->payload, command->frame + PROTOCOL_HEADER_SIZE,
            header->payload_length);
    }
    cache->ack = *ack;
}

static void process_start(
    const application_command_t *command, const protocol_header_t *header,
    control_state_t *state)
{
    protocol_start_t start;
    int64_t now_us = esp_timer_get_time();
    if (header->node_id != 0 || header->flags != 0 ||
        !protocol_decode_start_payload(command->frame + PROTOCOL_HEADER_SIZE,
            header->payload_length, &start) ||
        start.coordinator_start_us > INT64_MAX) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_INVALID_FRAME, PROTOCOL_MESSAGE_TYPE_START);
        return;
    }
    if (replay_control_if_cached(command, header, &state->start)) return;
    int64_t coordinator_start_us = (int64_t)start.coordinator_start_us;
    if ((start.node_minus_coordinator_us > 0 &&
            coordinator_start_us > INT64_MAX - start.node_minus_coordinator_us) ||
        (start.node_minus_coordinator_us < 0 &&
            coordinator_start_us < INT64_MIN - start.node_minus_coordinator_us)) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_INVALID_FRAME, PROTOCOL_MESSAGE_TYPE_START);
        return;
    }
    int64_t local_start_us = coordinator_start_us +
        start.node_minus_coordinator_us;
    protocol_control_ack_t ack = {
        .command_type = PROTOCOL_MESSAGE_TYPE_START,
        .result = PROTOCOL_CONTROL_RESULT_APPLIED,
        .command_sequence = header->sequence,
        .effective_local_us = (uint64_t)local_start_us,
    };
    if (state->capture_requested || local_start_us <= now_us ||
        sensor_source_schedule_start(configured_local_node_id, local_start_us) != ESP_OK)
        ack.result = PROTOCOL_CONTROL_RESULT_INVALID_STATE;
    else
        state->capture_requested = true;
    cache_control_result(command, header, &state->start, &ack);
    enqueue_control_ack(command->session_id, header->sequence, &ack);
    if (ack.result != PROTOCOL_CONTROL_RESULT_APPLIED) return;
    ESP_LOGI(TAG,
        "START armed: coordinator=%llu offset=%lld local=%lld",
        (unsigned long long)start.coordinator_start_us,
        (long long)start.node_minus_coordinator_us,
        (long long)local_start_us);
}

static void process_stop(
    const application_command_t *command, const protocol_header_t *header,
    control_state_t *state)
{
    if (header->node_id != 0 || header->flags != 0 ||
        header->payload_length != PROTOCOL_STOP_PAYLOAD_SIZE) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_INVALID_FRAME, PROTOCOL_MESSAGE_TYPE_STOP);
        return;
    }
    if (replay_control_if_cached(command, header, &state->stop)) return;
    int64_t stopped_at_us = esp_timer_get_time();
    protocol_control_ack_t ack = {
        .command_type = PROTOCOL_MESSAGE_TYPE_STOP,
        .result = state->capture_requested
            ? PROTOCOL_CONTROL_RESULT_APPLIED
            : PROTOCOL_CONTROL_RESULT_ALREADY_STOPPED,
        .command_sequence = header->sequence,
        .effective_local_us = (uint64_t)stopped_at_us,
    };
    (void)sensor_source_stop(configured_local_node_id);
    state->capture_requested = false;
    cache_control_result(command, header, &state->stop, &ack);
    enqueue_control_ack(command->session_id, header->sequence, &ack);
    ESP_LOGI(TAG, "STOP applied: sequence=%lu",
        (unsigned long)header->sequence);
}

/* Enforce replay rules and perform the deterministic per-hub sample transform. */
static void process_test_request(
    const application_command_t *command,
    const protocol_header_t *header,
    test_cache_entry_t cache[TEST_REQUEST_CACHE_SIZE],
    size_t *next_cache_index)
{
    imu_sample_t request;
    protocol_header_t decoded_header;

    if (header->node_id != 0 || header->flags != 0 ||
        !protocol_decode_test_request(command->frame, command->frame_length,
            &decoded_header, &request) ||
        (request.status & PROTOCOL_STATUS_MOCK_DATA) == 0) {
        ESP_LOGE(TAG, "Rejected malformed TEST_REQUEST");
        enqueue_error(command->session_id, header->sequence,
            header->node_id == 0 ? PROTOCOL_ERROR_INVALID_FRAME
                                 : PROTOCOL_ERROR_UNAUTHORIZED_NODE,
            PROTOCOL_MESSAGE_TYPE_TEST_REQUEST);
        return;
    }

    const uint8_t *request_payload = command->frame + PROTOCOL_HEADER_SIZE;
    for (size_t i = 0; i < TEST_REQUEST_CACHE_SIZE; ++i) {
        if (!cache[i].valid || cache[i].sequence != header->sequence) continue;

        if (memcmp(cache[i].request_payload, request_payload,
                PROTOCOL_TEST_PAYLOAD_SIZE) != 0) {
            ESP_LOGE(TAG, "Rejected TEST_REQUEST Sequence reuse: %lu",
                (unsigned long)header->sequence);
            enqueue_error(command->session_id, header->sequence,
                PROTOCOL_ERROR_SEQUENCE_REUSE,
                PROTOCOL_MESSAGE_TYPE_TEST_REQUEST);
            return;
        }

        ESP_LOGI(TAG, "Returning cached TEST_RESPONSE: sequence=%lu",
            (unsigned long)header->sequence);
        enqueue_test_response(command->session_id, &cache[i].response);
        return;
    }

    imu_sample_t response;
    if (!protocol_transform_test_sample(
            &request, configured_local_node_id, &response)) {
        enqueue_error(command->session_id, header->sequence,
            PROTOCOL_ERROR_INVALID_FRAME, PROTOCOL_MESSAGE_TYPE_TEST_REQUEST);
        return;
    }

    response.timestamp_us = (uint64_t)esp_timer_get_time();
    test_cache_entry_t *entry = &cache[*next_cache_index];
    entry->valid = true;
    entry->sequence = header->sequence;
    memcpy(entry->request_payload, request_payload, PROTOCOL_TEST_PAYLOAD_SIZE);
    entry->response = response;
    *next_cache_index = (*next_cache_index + 1) % TEST_REQUEST_CACHE_SIZE;

    ESP_LOGI(TAG, "Processed TEST_REQUEST: sequence=%lu ax=%d->%d gx=%d->%d",
        (unsigned long)header->sequence,
        request.ax, response.ax, request.gx, response.gx);
    enqueue_test_response(command->session_id, &response);
}

/* Validate the common header, then dispatch one complete downlink frame by type. */
static void process_frame(
    const application_command_t *command,
    test_cache_entry_t cache[TEST_REQUEST_CACHE_SIZE],
    size_t *next_cache_index, file_transfer_state_t *file_transfer,
    control_state_t *control_state)
{
    protocol_header_t header;
    if (!protocol_decode_header(command->frame, command->frame_length, &header)) {
        ESP_LOGE(TAG, "Application received an incomplete header");
        request_disconnect(command->session_id);
        return;
    }

    if (header.message_type == PROTOCOL_MESSAGE_TYPE_TEST_REQUEST) {
        process_test_request(command, &header, cache, next_cache_index);
        return;
    }

    if (header.message_type == PROTOCOL_MESSAGE_TYPE_SYNC_REQ) {
        process_sync_request(command, &header);
        return;
    }

    if (header.message_type == PROTOCOL_MESSAGE_TYPE_START) {
        process_start(command, &header, control_state);
        return;
    }

    if (header.message_type == PROTOCOL_MESSAGE_TYPE_STOP) {
        process_stop(command, &header, control_state);
        return;
    }

    if (header.message_type == PROTOCOL_MESSAGE_TYPE_BENCHMARK_CONTROL) {
        process_benchmark_control(command, &header);
        return;
    }

    if (header.message_type >= PROTOCOL_MESSAGE_TYPE_FILE_BEGIN &&
        header.message_type <= PROTOCOL_MESSAGE_TYPE_FILE_END) {
        process_file_frame(command, &header, file_transfer);
        return;
    }

    if (header.message_type == PROTOCOL_MESSAGE_TYPE_ERROR) {
        protocol_error_t error;
        if (header.node_id != 0 ||
            !protocol_decode_error_payload(command->frame + PROTOCOL_HEADER_SIZE,
                header.payload_length, &error)) {
            ESP_LOGE(TAG, "Received malformed ERROR from coordinator");
        } else {
            ESP_LOGE(TAG, "Received ERROR: code=%u offending_type=0x%02X",
                error.error_code, error.offending_message_type);
        }
        request_disconnect(command->session_id);
        return;
    }

    ESP_LOGE(TAG, "Unexpected downlink message: type=0x%02X node=%u payload=%u",
        header.message_type, header.node_id, header.payload_length);
    enqueue_error(command->session_id, header.sequence,
        PROTOCOL_ERROR_UNEXPECTED_MESSAGE, header.message_type);
}

/* Own all Session-scoped command state so socket and callback tasks stay stateless. */
static void application_task(void *argument)
{
    (void)argument;
    uint32_t active_session_id = 0;
    test_cache_entry_t test_cache[TEST_REQUEST_CACHE_SIZE] = {0};
    size_t next_cache_index = 0;
    file_transfer_state_t file_transfer = {0};
    control_state_t control_state = {0};

    while (true) {
        application_command_t command;
        if (xQueueReceive(command_queue, &command, pdMS_TO_TICKS(250)) != pdPASS) {
            if (file_transfer.active &&
                esp_timer_get_time() - file_transfer.last_activity_us >=
                    (int64_t)FILE_TRANSFER_TIMEOUT_MS * 1000) {
                application_command_t timeout_command = {
                    .session_id = active_session_id,
                };
                protocol_header_t timeout_header = {
                    .sequence = 0,
                };
                ESP_LOGW(TAG, "File transfer timed out: id=%lu offset=%lu",
                    (unsigned long)file_transfer.file_id,
                    (unsigned long)file_transfer.next_offset);
                enqueue_file_response(&timeout_command, &timeout_header,
                    PROTOCOL_FILE_RESULT_ABORTED, NULL, file_transfer.file_id,
                    file_transfer.next_offset, false);
                reset_file_transfer(&file_transfer);
            }
            continue;
        }

        if (command.type == APPLICATION_COMMAND_BEGIN_SESSION) {
            active_session_id = command.session_id;
            memset(test_cache, 0, sizeof(test_cache));
            next_cache_index = 0;
            reset_file_transfer(&file_transfer);
            memset(&control_state, 0, sizeof(control_state));
            (void)sensor_source_stop(configured_local_node_id);
            ESP_LOGI(TAG, "Application session started: %lu",
                (unsigned long)active_session_id);
            continue;
        }

        if (command.type == APPLICATION_COMMAND_END_SESSION) {
            if (active_session_id == command.session_id) {
                active_session_id = 0;
                memset(test_cache, 0, sizeof(test_cache));
                next_cache_index = 0;
                if (file_transfer.active)
                    ESP_LOGW(TAG, "File transfer aborted by session end at offset %lu",
                        (unsigned long)file_transfer.next_offset);
                reset_file_transfer(&file_transfer);
                memset(&control_state, 0, sizeof(control_state));
                (void)sensor_source_stop(configured_local_node_id);
                ESP_LOGI(TAG, "Application session ended");
            }
            continue;
        }

        if (command.session_id == 0 || command.session_id != active_session_id) {
            ESP_LOGW(TAG, "Discarded stale command for session %lu",
                (unsigned long)command.session_id);
            continue;
        }

        process_frame(&command, test_cache, &next_cache_index, &file_transfer,
            &control_state);
    }
}

/* Create the bounded queues and singleton command-processing task. */
esp_err_t application_start(uint8_t local_node_id)
{
    if (local_node_id < 1 || local_node_id > 6) return ESP_ERR_INVALID_ARG;
    if (application_task_handle != NULL) return ESP_ERR_INVALID_STATE;

    configured_local_node_id = local_node_id;
    command_queue = xQueueCreate(APPLICATION_COMMAND_QUEUE_LENGTH,
        sizeof(application_command_t));
    response_queue = xQueueCreate(APPLICATION_RESPONSE_QUEUE_LENGTH,
        sizeof(application_response_t));

    if (command_queue == NULL || response_queue == NULL) {
        if (command_queue != NULL) vQueueDelete(command_queue);
        if (response_queue != NULL) vQueueDelete(response_queue);
        command_queue = NULL;
        response_queue = NULL;
        configured_local_node_id = 0;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(application_task, "application",
        APPLICATION_TASK_STACK_SIZE, NULL, APPLICATION_TASK_PRIORITY,
        &application_task_handle);
    if (result != pdPASS) {
        vQueueDelete(command_queue);
        vQueueDelete(response_queue);
        command_queue = NULL;
        response_queue = NULL;
        application_task_handle = NULL;
        configured_local_node_id = 0;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Queue an explicit boundary that clears replay, benchmark, and file state. */
bool application_begin_session(uint32_t session_id)
{
    application_command_t command = {
        .type = APPLICATION_COMMAND_BEGIN_SESSION, .session_id = session_id,
    };
    if (session_id == 0 || command_queue == NULL || response_queue == NULL) return false;
    xQueueReset(command_queue);
    xQueueReset(response_queue);
    return xQueueSend(command_queue, &command, 0) == pdPASS;
}

/* Copy a complete frame into owned queue storage; the caller may then reuse its buffer. */
bool application_submit_frame(
    uint32_t session_id, const uint8_t *frame, size_t frame_length)
{
    application_command_t command = {
        .type = APPLICATION_COMMAND_FRAME,
        .session_id = session_id,
        .frame_length = frame_length,
        .received_at_us = (uint64_t)esp_timer_get_time(),
    };
    if (command_queue == NULL || session_id == 0 || frame == NULL ||
        frame_length < PROTOCOL_HEADER_SIZE ||
        frame_length > PROTOCOL_MAX_FRAME_SIZE) return false;
    memcpy(command.frame, frame, frame_length);
    return xQueueSend(command_queue, &command, 0) == pdPASS;
}

/* Queue Session teardown so no pending state can leak into a reconnection. */
void application_end_session(uint32_t session_id)
{
    application_command_t command = {
        .type = APPLICATION_COMMAND_END_SESSION, .session_id = session_id,
    };
    if (command_queue == NULL || response_queue == NULL || session_id == 0) return;
    xQueueReset(command_queue);
    xQueueReset(response_queue);
    (void)xQueueSend(command_queue, &command, 0);
}

/* Let the transport poll logical results while retaining exclusive socket writes. */
bool application_receive_response(
    application_response_t *response, TickType_t ticks_to_wait)
{
    return response != NULL && response_queue != NULL &&
        xQueueReceive(response_queue, response, ticks_to_wait) == pdPASS;
}
