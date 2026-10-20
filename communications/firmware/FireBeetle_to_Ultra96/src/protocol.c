/**
 * @file protocol.c
 * @brief Serialization for the ESP32–Ultra96 application protocol.
 *
 * This module converts logical IMU samples into canonical wire-format DATA
 * frames. It deliberately writes every multibyte field byte by byte in
 * network byte order instead of transmitting a packed C structure.
 *
 * Version-1 DATA frame layout:
 *
 *   Bytes 0-23:  fixed protocol header
 *   Bytes 24-39: signed IMU values, battery voltage, and status
 *
 * The module is independent of Wi-Fi, TCP, ESP-NOW, queues, and the selected
 * sensor implementation.
 */
#include "protocol.h"

#include <string.h>
#include <limits.h>
#include <math.h>

/* Explicit byte helpers keep wire layout independent of ESP32 alignment and endianness. */
static uint16_t read_u16_be(const uint8_t *source)
{
    return ((uint16_t)source[0] << 8) |
        (uint16_t)source[1];
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) |
        ((uint32_t)source[1] << 16) |
        ((uint32_t)source[2] << 8) |
        (uint32_t)source[3];
}

static uint64_t read_u64_be(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56) |
        ((uint64_t)source[1] << 48) |
        ((uint64_t)source[2] << 40) |
        ((uint64_t)source[3] << 32) |
        ((uint64_t)source[4] << 24) |
        ((uint64_t)source[5] << 16) |
        ((uint64_t)source[6] << 8) |
        (uint64_t)source[7];
}

static void write_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static void write_u64_be(uint8_t *destination, uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56);
    destination[1] = (uint8_t)(value >> 48);
    destination[2] = (uint8_t)(value >> 40);
    destination[3] = (uint8_t)(value >> 32);
    destination[4] = (uint8_t)(value >> 24);
    destination[5] = (uint8_t)(value >> 16);
    destination[6] = (uint8_t)(value >> 8);
    destination[7] = (uint8_t)value;
}

static float read_f32_be(const uint8_t *source)
{
    uint32_t bits = read_u32_be(source);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_f32_be(uint8_t *destination, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_u32_be(destination, bits);
}

/* Shared DATA-shaped encoder used by request and response message types. */
static void encode_imu_frame(
    uint8_t packet[PROTOCOL_TEST_PACKET_SIZE],
    uint8_t message_type,
    const imu_sample_t *sample,
    uint32_t session_id)
{
    memset(packet, 0, PROTOCOL_TEST_PACKET_SIZE);
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = message_type;
    packet[4] = sample->node_id;
    packet[5] = sample->flags;
    write_u16_be(&packet[6], PROTOCOL_TEST_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], sample->sequence);
    write_u64_be(&packet[16], sample->timestamp_us);
    write_u16_be(&packet[24], (uint16_t)sample->ax);
    write_u16_be(&packet[26], (uint16_t)sample->ay);
    write_u16_be(&packet[28], (uint16_t)sample->az);
    write_u16_be(&packet[30], (uint16_t)sample->gx);
    write_u16_be(&packet[32], (uint16_t)sample->gy);
    write_u16_be(&packet[34], (uint16_t)sample->gz);
    write_u16_be(&packet[36], sample->battery_mv);
    write_u16_be(&packet[38], sample->status);
}

/* Validate the complete fixed-size frame before exposing any decoded sample. */
static bool decode_imu_frame(
    const uint8_t *frame,
    size_t frame_length,
    uint8_t expected_message_type,
    protocol_header_t *header,
    imu_sample_t *sample)
{
    if (frame == NULL || header == NULL || sample == NULL ||
        frame_length != PROTOCOL_TEST_PACKET_SIZE ||
        !protocol_decode_header(frame, frame_length, header) ||
        header->magic != PROTOCOL_MAGIC ||
        header->version != PROTOCOL_VERSION ||
        header->message_type != expected_message_type ||
        header->payload_length != PROTOCOL_TEST_PAYLOAD_SIZE) {
        return false;
    }
    sample->node_id = header->node_id;
    sample->flags = header->flags;
    sample->sequence = header->sequence;
    sample->timestamp_us = header->timestamp_us;
    sample->ax = (int16_t)read_u16_be(&frame[24]);
    sample->ay = (int16_t)read_u16_be(&frame[26]);
    sample->az = (int16_t)read_u16_be(&frame[28]);
    sample->gx = (int16_t)read_u16_be(&frame[30]);
    sample->gy = (int16_t)read_u16_be(&frame[32]);
    sample->gz = (int16_t)read_u16_be(&frame[34]);
    sample->battery_mv = read_u16_be(&frame[36]);
    sample->status = read_u16_be(&frame[38]);
    return true;
}

/* DATA uses the canonical IMU layout and the source's original capture metadata. */
void protocol_encode_data(
    uint8_t packet[PROTOCOL_DATA_PACKET_SIZE],
    const imu_sample_t *sample,
    uint32_t session_id)
{
    memset(packet, 0, PROTOCOL_DATA_PACKET_SIZE);

    /* 24-byte fixed header */
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_DATA;
    packet[4] = sample->node_id;
    packet[5] = sample->flags;
    write_u16_be(&packet[6], PROTOCOL_DATA_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], sample->sequence);
    write_u64_be(&packet[16], sample->timestamp_us);

    /* 16-byte DATA payload */
    write_u16_be(&packet[24], (uint16_t)sample->ax);
    write_u16_be(&packet[26], (uint16_t)sample->ay);
    write_u16_be(&packet[28], (uint16_t)sample->az);
    write_u16_be(&packet[30], (uint16_t)sample->gx);
    write_u16_be(&packet[32], (uint16_t)sample->gy);
    write_u16_be(&packet[34], (uint16_t)sample->gz);
    write_u16_be(&packet[36], sample->battery_mv);
    write_u16_be(&packet[38], sample->status);
}

bool protocol_encode_physical_data(
    uint8_t packet[PROTOCOL_PHYSICAL_DATA_PACKET_SIZE],
    const protocol_physical_sample_t *sample, uint32_t session_id)
{
    if (packet == NULL || sample == NULL || session_id == 0 ||
        !isfinite(sample->ax) || !isfinite(sample->ay) ||
        !isfinite(sample->az) || !isfinite(sample->gx) ||
        !isfinite(sample->gy) || !isfinite(sample->gz)) return false;
    memset(packet, 0, PROTOCOL_PHYSICAL_DATA_PACKET_SIZE);
    write_u16_be(packet, PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_PHYSICAL_DATA;
    packet[4] = sample->node_id;
    packet[5] = sample->flags;
    write_u16_be(packet + 6, PROTOCOL_PHYSICAL_DATA_PAYLOAD_SIZE);
    write_u32_be(packet + 8, session_id);
    write_u32_be(packet + 12, sample->sequence);
    write_u64_be(packet + 16, sample->timestamp_us);
    write_f32_be(packet + 24, sample->ax);
    write_f32_be(packet + 28, sample->ay);
    write_f32_be(packet + 32, sample->az);
    write_f32_be(packet + 36, sample->gx);
    write_f32_be(packet + 40, sample->gy);
    write_f32_be(packet + 44, sample->gz);
    write_u16_be(packet + 48, sample->status);
    return true;
}

bool protocol_decode_physical_data(
    const uint8_t *frame, size_t frame_length,
    protocol_header_t *header, protocol_physical_sample_t *sample)
{
    if (frame == NULL || header == NULL || sample == NULL ||
        frame_length != PROTOCOL_PHYSICAL_DATA_PACKET_SIZE ||
        !protocol_decode_header(frame, frame_length, header) ||
        header->magic != PROTOCOL_MAGIC || header->version != PROTOCOL_VERSION ||
        header->message_type != PROTOCOL_MESSAGE_TYPE_PHYSICAL_DATA ||
        header->payload_length != PROTOCOL_PHYSICAL_DATA_PAYLOAD_SIZE) return false;
    memset(sample, 0, sizeof(*sample));
    sample->node_id = header->node_id;
    sample->flags = header->flags;
    sample->sequence = header->sequence;
    sample->timestamp_us = header->timestamp_us;
    sample->ax = read_f32_be(frame + 24);
    sample->ay = read_f32_be(frame + 28);
    sample->az = read_f32_be(frame + 32);
    sample->gx = read_f32_be(frame + 36);
    sample->gy = read_f32_be(frame + 40);
    sample->gz = read_f32_be(frame + 44);
    sample->status = read_u16_be(frame + 48);
    return isfinite(sample->ax) && isfinite(sample->ay) &&
        isfinite(sample->az) && isfinite(sample->gx) &&
        isfinite(sample->gy) && isfinite(sample->gz);
}

void protocol_encode_hello(
    uint8_t packet[PROTOCOL_HELLO_PACKET_SIZE],
    const protocol_hello_t *hello,
    uint64_t timestamp_us)
{
    memset(packet, 0, PROTOCOL_HELLO_PACKET_SIZE);

    /* HELLO uses Session ID zero because the Ultra96 has not assigned one. */
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_HELLO;
    packet[4] = hello->hub_node_id;
    packet[5] = 0;
    write_u16_be(&packet[6], PROTOCOL_HELLO_PAYLOAD_SIZE);
    write_u32_be(&packet[8], 0);
    write_u32_be(&packet[12], 0);
    write_u64_be(&packet[16], timestamp_us);

    packet[24] = hello->hub_node_id;
    packet[25] = hello->firmware_major;
    packet[26] = hello->firmware_minor;
    packet[27] = hello->firmware_patch;
    write_u32_be(&packet[28], hello->boot_id);
    write_u16_be(&packet[32], hello->supported_sampling_rates);
    packet[34] = hello->assigned_node_mask;
    packet[35] = 0; /* Reserved; transmit as zero in protocol version 1. */
    memcpy(
        &packet[36],
        hello->device_token,
        PROTOCOL_DEVICE_TOKEN_SIZE
    );
}

void protocol_encode_heartbeat(
    uint8_t packet[PROTOCOL_HEARTBEAT_PACKET_SIZE],
    const protocol_heartbeat_t *heartbeat,
    uint32_t session_id)
{
    memset(packet, 0, PROTOCOL_HEARTBEAT_PACKET_SIZE);

    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_HEARTBEAT;
    packet[4] = heartbeat->node_id;
    packet[5] = heartbeat->flags;
    write_u16_be(&packet[6], PROTOCOL_HEARTBEAT_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], heartbeat->sequence);
    write_u64_be(&packet[16], heartbeat->timestamp_us);

    write_u16_be(&packet[24], heartbeat->battery_mv);
    write_u16_be(&packet[26], heartbeat->status);
    write_u32_be(&packet[28], heartbeat->overflow_count);
}

/* Decode only the common header; message-specific policy remains with the caller. */
bool protocol_decode_header(
    const uint8_t *data,
    size_t data_length,
    protocol_header_t *header)
{
    if (data == NULL ||
        header == NULL ||
        data_length < PROTOCOL_HEADER_SIZE) {

        return false;
    }

    header->magic = read_u16_be(&data[0]);
    header->version = data[2];
    header->message_type = data[3];
    header->node_id = data[4];
    header->flags = data[5];
    header->payload_length = read_u16_be(&data[6]);
    header->session_id = read_u32_be(&data[8]);
    header->sequence = read_u32_be(&data[12]);
    header->timestamp_us = read_u64_be(&data[16]);

    return true;
}

bool protocol_decode_error_payload(
    const uint8_t *payload,
    size_t payload_length,
    protocol_error_t *error)
{
    if (payload == NULL ||
        error == NULL ||
        payload_length != PROTOCOL_ERROR_PAYLOAD_SIZE ||
        payload[3] != 0) {

        return false;
    }

    error->error_code = read_u16_be(&payload[0]);
    error->offending_message_type = payload[2];
    return true;
}

void protocol_encode_error(
    uint8_t packet[PROTOCOL_ERROR_PACKET_SIZE],
    uint8_t node_id,
    uint32_t session_id,
    uint32_t sequence,
    uint64_t timestamp_us,
    uint16_t error_code,
    uint8_t offending_message_type)
{
    memset(packet, 0, PROTOCOL_ERROR_PACKET_SIZE);
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_ERROR;
    packet[4] = node_id;
    write_u16_be(&packet[6], PROTOCOL_ERROR_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], sequence);
    write_u64_be(&packet[16], timestamp_us);
    write_u16_be(&packet[24], error_code);
    packet[26] = offending_message_type;
}

bool protocol_decode_sync_request_payload(
    const uint8_t *payload, size_t payload_length,
    uint64_t *coordinator_send_us)
{
    if (payload == NULL || coordinator_send_us == NULL ||
        payload_length != PROTOCOL_SYNC_REQ_PAYLOAD_SIZE) return false;
    *coordinator_send_us = read_u64_be(payload);
    return true;
}

void protocol_encode_sync_response(
    uint8_t packet[PROTOCOL_SYNC_RESP_PACKET_SIZE],
    uint8_t node_id, uint32_t session_id, uint32_t sequence,
    const protocol_sync_response_t *response)
{
    memset(packet, 0, PROTOCOL_SYNC_RESP_PACKET_SIZE);
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_SYNC_RESP;
    packet[4] = node_id;
    write_u16_be(&packet[6], PROTOCOL_SYNC_RESP_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], sequence);
    write_u64_be(&packet[16], response->node_send_us);
    write_u64_be(&packet[24], response->coordinator_send_us);
    write_u64_be(&packet[32], response->node_receive_us);
    write_u64_be(&packet[40], response->node_send_us);
}

bool protocol_decode_start_payload(
    const uint8_t *payload, size_t payload_length,
    protocol_start_t *start)
{
    if (payload == NULL || start == NULL ||
        payload_length != PROTOCOL_START_PAYLOAD_SIZE) return false;
    start->coordinator_start_us = read_u64_be(&payload[0]);
    start->node_minus_coordinator_us = (int64_t)read_u64_be(&payload[8]);
    return true;
}

void protocol_encode_control_ack(
    uint8_t packet[PROTOCOL_CONTROL_ACK_PACKET_SIZE],
    uint8_t node_id, uint32_t session_id, uint32_t response_sequence,
    uint64_t timestamp_us, const protocol_control_ack_t *ack)
{
    memset(packet, 0, PROTOCOL_CONTROL_ACK_PACKET_SIZE);
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_CONTROL_ACK;
    packet[4] = node_id;
    write_u16_be(&packet[6], PROTOCOL_CONTROL_ACK_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], response_sequence);
    write_u64_be(&packet[16], timestamp_us);
    packet[24] = ack->command_type;
    packet[25] = ack->result;
    write_u16_be(&packet[26], 0);
    write_u32_be(&packet[28], ack->command_sequence);
    write_u64_be(&packet[32], ack->effective_local_us);
}

bool protocol_decode_start_flush_payload(
    const uint8_t *payload, size_t payload_length,
    protocol_start_flush_t *start)
{
    if (payload == NULL || start == NULL ||
        payload_length != PROTOCOL_START_FLUSH_PAYLOAD_SIZE) return false;
    /* Bytes 21..23 are reserved so the final trigger Sequence is aligned. */
    if (payload[21] != 0 || payload[22] != 0 || payload[23] != 0) return false;
    start->coordinator_t0_us = read_u64_be(payload);
    start->node_minus_coordinator_us = (int64_t)read_u64_be(payload + 8);
    start->history_duration_us = read_u32_be(payload + 16);
    start->trigger_node_id = payload[20];
    start->trigger_sequence = read_u32_be(payload + 24);
    return true;
}

void protocol_encode_activity_trigger(
    uint8_t packet[PROTOCOL_ACTIVITY_TRIGGER_PACKET_SIZE],
    uint8_t node_id, uint32_t session_id, uint32_t sequence,
    uint64_t detected_timestamp_us,
    const protocol_activity_trigger_t *trigger)
{
    memset(packet, 0, PROTOCOL_ACTIVITY_TRIGGER_PACKET_SIZE);
    write_u16_be(packet, PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_ACTIVITY_TRIGGER;
    packet[4] = node_id;
    write_u16_be(packet + 6, PROTOCOL_ACTIVITY_TRIGGER_PAYLOAD_SIZE);
    write_u32_be(packet + 8, session_id);
    write_u32_be(packet + 12, sequence);
    write_u64_be(packet + 16, detected_timestamp_us);
    write_u32_be(packet + 24, trigger->boot_id);
    write_u32_be(packet + 28, trigger->arm_generation);
    write_u64_be(packet + 32, trigger->peak_timestamp_us);
}

bool protocol_decode_activity_trigger(
    const uint8_t *frame, size_t frame_length,
    protocol_header_t *header, protocol_activity_trigger_t *trigger)
{
    if (frame == NULL || header == NULL || trigger == NULL ||
        frame_length != PROTOCOL_ACTIVITY_TRIGGER_PACKET_SIZE ||
        !protocol_decode_header(frame, frame_length, header) ||
        header->magic != PROTOCOL_MAGIC || header->version != PROTOCOL_VERSION ||
        header->message_type != PROTOCOL_MESSAGE_TYPE_ACTIVITY_TRIGGER ||
        header->payload_length != PROTOCOL_ACTIVITY_TRIGGER_PAYLOAD_SIZE) return false;
    trigger->boot_id = read_u32_be(frame + 24);
    trigger->arm_generation = read_u32_be(frame + 28);
    trigger->peak_timestamp_us = read_u64_be(frame + 32);
    return trigger->boot_id != 0 && trigger->arm_generation != 0 &&
        trigger->peak_timestamp_us <= header->timestamp_us;
}

bool protocol_decode_data(
    const uint8_t *frame,
    size_t frame_length,
    protocol_header_t *header,
    imu_sample_t *sample)
{
    if (frame == NULL || header == NULL || sample == NULL ||
        frame_length != PROTOCOL_DATA_PACKET_SIZE ||
        !protocol_decode_header(frame, frame_length, header) ||
        header->magic != PROTOCOL_MAGIC ||
        header->version != PROTOCOL_VERSION ||
        header->message_type != PROTOCOL_MESSAGE_TYPE_DATA ||
        header->payload_length != PROTOCOL_DATA_PAYLOAD_SIZE) {

        return false;
    }

    sample->node_id = header->node_id;
    sample->flags = header->flags;
    sample->sequence = header->sequence;
    sample->timestamp_us = header->timestamp_us;
    sample->ax = (int16_t)read_u16_be(&frame[24]);
    sample->ay = (int16_t)read_u16_be(&frame[26]);
    sample->az = (int16_t)read_u16_be(&frame[28]);
    sample->gx = (int16_t)read_u16_be(&frame[30]);
    sample->gy = (int16_t)read_u16_be(&frame[32]);
    sample->gz = (int16_t)read_u16_be(&frame[34]);
    sample->battery_mv = read_u16_be(&frame[36]);
    sample->status = read_u16_be(&frame[38]);
    return true;
}

void protocol_encode_test_request(
    uint8_t packet[PROTOCOL_TEST_PACKET_SIZE],
    const imu_sample_t *sample,
    uint32_t session_id)
{
    encode_imu_frame(packet, PROTOCOL_MESSAGE_TYPE_TEST_REQUEST, sample, session_id);
}

void protocol_encode_test_response(
    uint8_t packet[PROTOCOL_TEST_PACKET_SIZE],
    const imu_sample_t *sample,
    uint32_t session_id)
{
    encode_imu_frame(packet, PROTOCOL_MESSAGE_TYPE_TEST_RESPONSE, sample, session_id);
}

bool protocol_decode_test_request(
    const uint8_t *frame, size_t frame_length,
    protocol_header_t *header, imu_sample_t *sample)
{
    return decode_imu_frame(frame, frame_length,
        PROTOCOL_MESSAGE_TYPE_TEST_REQUEST, header, sample);
}

bool protocol_decode_test_response(
    const uint8_t *frame, size_t frame_length,
    protocol_header_t *header, imu_sample_t *sample)
{
    return decode_imu_frame(frame, frame_length,
        PROTOCOL_MESSAGE_TYPE_TEST_RESPONSE, header, sample);
}

int16_t protocol_saturating_add_i16(int16_t value, int16_t increment)
{
    int32_t sum = (int32_t)value + (int32_t)increment;
    if (sum > INT16_MAX) return INT16_MAX;
    if (sum < INT16_MIN) return INT16_MIN;
    return (int16_t)sum;
}

/* Apply the deterministic demo transform with saturation instead of overflow. */
bool protocol_transform_test_sample(
    const imu_sample_t *request, uint8_t local_node_id, imu_sample_t *response)
{
    if (request == NULL || response == NULL) return false;
    *response = *request;
    response->node_id = local_node_id;
    response->ax = protocol_saturating_add_i16(request->ax, local_node_id);
    response->gx = protocol_saturating_add_i16(request->gx, 100);
    response->status = request->status |
        PROTOCOL_STATUS_MOCK_DATA | PROTOCOL_STATUS_TEST_REQUEST_PROCESSED;
    return true;
}

void protocol_encode_benchmark_control_payload(
    uint8_t payload[PROTOCOL_BENCHMARK_CONTROL_PAYLOAD_SIZE],
    const protocol_benchmark_control_t *control)
{
    payload[0] = control->action;
    payload[1] = 0;
    write_u16_be(&payload[2], control->batch_frames);
    write_u32_be(&payload[4], control->duration_ms);
}

bool protocol_decode_benchmark_control_payload(
    const uint8_t *payload, size_t payload_length,
    protocol_benchmark_control_t *control)
{
    if (payload == NULL || control == NULL ||
        payload_length != PROTOCOL_BENCHMARK_CONTROL_PAYLOAD_SIZE ||
        payload[1] != 0 || payload[0] > PROTOCOL_BENCHMARK_ACTION_START) return false;
    control->action = payload[0];
    control->batch_frames = read_u16_be(&payload[2]);
    control->duration_ms = read_u32_be(&payload[4]);
    return true;
}

void protocol_encode_benchmark_report_payload(
    uint8_t payload[PROTOCOL_BENCHMARK_REPORT_PAYLOAD_SIZE],
    const protocol_benchmark_report_t *report)
{
    write_u32_be(&payload[0], report->control_sequence);
    write_u32_be(&payload[4], report->elapsed_ms);
    write_u32_be(&payload[8], report->frames_sent);
    write_u32_be(&payload[12], report->application_bytes_sent);
    write_u32_be(&payload[16], report->queue_drops);
    write_u32_be(&payload[20], report->send_failures);
}

bool protocol_decode_benchmark_report_payload(
    const uint8_t *payload, size_t payload_length,
    protocol_benchmark_report_t *report)
{
    if (payload == NULL || report == NULL ||
        payload_length != PROTOCOL_BENCHMARK_REPORT_PAYLOAD_SIZE) return false;
    report->control_sequence = read_u32_be(&payload[0]);
    report->elapsed_ms = read_u32_be(&payload[4]);
    report->frames_sent = read_u32_be(&payload[8]);
    report->application_bytes_sent = read_u32_be(&payload[12]);
    report->queue_drops = read_u32_be(&payload[16]);
    report->send_failures = read_u32_be(&payload[20]);
    return true;
}

void protocol_encode_benchmark_report(
    uint8_t packet[PROTOCOL_BENCHMARK_REPORT_PACKET_SIZE],
    uint8_t node_id, uint32_t session_id, uint32_t sequence,
    uint64_t timestamp_us, const protocol_benchmark_report_t *report)
{
    memset(packet, 0, PROTOCOL_BENCHMARK_REPORT_PACKET_SIZE);
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_BENCHMARK_REPORT;
    packet[4] = node_id;
    write_u16_be(&packet[6], PROTOCOL_BENCHMARK_REPORT_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], sequence);
    write_u64_be(&packet[16], timestamp_us);
    protocol_encode_benchmark_report_payload(&packet[PROTOCOL_HEADER_SIZE], report);
}

/* File helpers serialize metadata only; transfer ordering is enforced by application.c. */
void protocol_encode_file_begin_payload(
    uint8_t payload[PROTOCOL_FILE_BEGIN_PAYLOAD_SIZE],
    const protocol_file_begin_t *begin)
{
    write_u32_be(&payload[0], begin->file_id);
    write_u32_be(&payload[4], begin->total_size);
    write_u16_be(&payload[8], begin->chunk_data_size);
    write_u16_be(&payload[10], 0);
    memcpy(&payload[12], begin->expected_sha256, PROTOCOL_SHA256_SIZE);
}

bool protocol_decode_file_begin_payload(
    const uint8_t *payload, size_t payload_length, protocol_file_begin_t *begin)
{
    if (payload == NULL || begin == NULL ||
        payload_length != PROTOCOL_FILE_BEGIN_PAYLOAD_SIZE ||
        read_u16_be(&payload[10]) != 0) return false;
    begin->file_id = read_u32_be(&payload[0]);
    begin->total_size = read_u32_be(&payload[4]);
    begin->chunk_data_size = read_u16_be(&payload[8]);
    memcpy(begin->expected_sha256, &payload[12], PROTOCOL_SHA256_SIZE);
    return true;
}

bool protocol_encode_file_chunk_payload(
    uint8_t *payload, size_t payload_capacity,
    const protocol_file_chunk_t *chunk, size_t *encoded_length)
{
    size_t required;
    if (payload == NULL || chunk == NULL || encoded_length == NULL ||
        chunk->data == NULL || chunk->data_length > PROTOCOL_FILE_CHUNK_MAX_DATA_SIZE) return false;
    required = PROTOCOL_FILE_CHUNK_PREFIX_SIZE + chunk->data_length;
    if (payload_capacity < required) return false;
    write_u32_be(&payload[0], chunk->file_id);
    write_u32_be(&payload[4], chunk->offset);
    write_u16_be(&payload[8], chunk->data_length);
    write_u16_be(&payload[10], 0);
    write_u32_be(&payload[12], chunk->crc32);
    memcpy(&payload[16], chunk->data, chunk->data_length);
    *encoded_length = required;
    return true;
}

bool protocol_decode_file_chunk_payload(
    const uint8_t *payload, size_t payload_length, protocol_file_chunk_t *chunk)
{
    uint16_t data_length;
    if (payload == NULL || chunk == NULL ||
        payload_length < PROTOCOL_FILE_CHUNK_PREFIX_SIZE ||
        payload_length > PROTOCOL_MAX_PAYLOAD_SIZE ||
        read_u16_be(&payload[10]) != 0) return false;
    data_length = read_u16_be(&payload[8]);
    if (data_length > PROTOCOL_FILE_CHUNK_MAX_DATA_SIZE ||
        payload_length != (size_t)PROTOCOL_FILE_CHUNK_PREFIX_SIZE + data_length) return false;
    chunk->file_id = read_u32_be(&payload[0]);
    chunk->offset = read_u32_be(&payload[4]);
    chunk->data_length = data_length;
    chunk->crc32 = read_u32_be(&payload[12]);
    chunk->data = &payload[16];
    return true;
}

void protocol_encode_file_end_payload(
    uint8_t payload[PROTOCOL_FILE_END_PAYLOAD_SIZE], const protocol_file_end_t *end)
{
    write_u32_be(&payload[0], end->file_id);
    write_u32_be(&payload[4], end->total_size);
    memcpy(&payload[8], end->expected_sha256, PROTOCOL_SHA256_SIZE);
}

bool protocol_decode_file_end_payload(
    const uint8_t *payload, size_t payload_length, protocol_file_end_t *end)
{
    if (payload == NULL || end == NULL ||
        payload_length != PROTOCOL_FILE_END_PAYLOAD_SIZE) return false;
    end->file_id = read_u32_be(&payload[0]);
    end->total_size = read_u32_be(&payload[4]);
    memcpy(end->expected_sha256, &payload[8], PROTOCOL_SHA256_SIZE);
    return true;
}

void protocol_encode_file_result_payload(
    uint8_t payload[PROTOCOL_FILE_RESULT_PAYLOAD_SIZE],
    const protocol_file_result_t *result)
{
    write_u32_be(&payload[0], result->file_id);
    write_u16_be(&payload[4], result->result_code);
    write_u16_be(&payload[6], 0);
    write_u32_be(&payload[8], result->bytes_received);
    memcpy(&payload[12], result->calculated_sha256, PROTOCOL_SHA256_SIZE);
}

bool protocol_decode_file_result_payload(
    const uint8_t *payload, size_t payload_length, protocol_file_result_t *result)
{
    if (payload == NULL || result == NULL ||
        payload_length != PROTOCOL_FILE_RESULT_PAYLOAD_SIZE ||
        read_u16_be(&payload[6]) != 0) return false;
    result->file_id = read_u32_be(&payload[0]);
    result->result_code = read_u16_be(&payload[4]);
    result->bytes_received = read_u32_be(&payload[8]);
    memcpy(result->calculated_sha256, &payload[12], PROTOCOL_SHA256_SIZE);
    return true;
}

void protocol_encode_file_result(
    uint8_t packet[PROTOCOL_FILE_RESULT_PACKET_SIZE],
    uint8_t node_id, uint32_t session_id, uint32_t sequence,
    uint64_t timestamp_us, const protocol_file_result_t *result)
{
    memset(packet, 0, PROTOCOL_FILE_RESULT_PACKET_SIZE);
    write_u16_be(&packet[0], PROTOCOL_MAGIC);
    packet[2] = PROTOCOL_VERSION;
    packet[3] = PROTOCOL_MESSAGE_TYPE_FILE_RESULT;
    packet[4] = node_id;
    write_u16_be(&packet[6], PROTOCOL_FILE_RESULT_PAYLOAD_SIZE);
    write_u32_be(&packet[8], session_id);
    write_u32_be(&packet[12], sequence);
    write_u64_be(&packet[16], timestamp_us);
    protocol_encode_file_result_payload(&packet[PROTOCOL_HEADER_SIZE], result);
}
