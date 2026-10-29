#include "protocol.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static void assert_bytes(const uint8_t *actual, const uint8_t *expected, size_t size)
{
    assert(memcmp(actual, expected, size) == 0);
}

static void test_existing_data_golden_vector(void)
{
    const imu_sample_t input = {
        .node_id = 3, .flags = 0x5a, .sequence = 0x01020304,
        .timestamp_us = UINT64_C(0x0102030405060708),
        .ax = INT16_MIN, .ay = INT16_MAX, .az = -1,
        .gx = 0, .gy = 1, .gz = -2, .status = 0xc000,
    };
    const uint8_t expected[PROTOCOL_DATA_PACKET_SIZE] = {
        0xa5,0x5a,0x01,0x10,0x03,0x5a,0x00,0x0e,
        0x11,0x22,0x33,0x44,0x01,0x02,0x03,0x04,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x80,0x00,0x7f,0xff,0xff,0xff,0x00,0x00,
        0x00,0x01,0xff,0xfe,0xc0,0x00,
    };
    uint8_t encoded[PROTOCOL_DATA_PACKET_SIZE];
    protocol_header_t header;
    imu_sample_t decoded;

    protocol_encode_data(encoded, &input, 0x11223344);
    assert_bytes(encoded, expected, sizeof(expected));
    assert(protocol_decode_data(encoded, sizeof(encoded), &header, &decoded));
    assert(decoded.ax == INT16_MIN && decoded.ay == INT16_MAX);
    assert(decoded.gz == -2 && decoded.status == 0xc000);
}

static void test_test_messages_and_transform(void)
{
    const imu_sample_t request = {
        .node_id = 0, .flags = 0, .sequence = 0xa1b2c3d4,
        .timestamp_us = UINT64_C(0x1020304050607080),
        .ax = INT16_MAX, .ay = INT16_MIN, .az = -123,
        .gx = 32700, .gy = 456, .gz = -789,
        .status = PROTOCOL_STATUS_MOCK_DATA,
    };
    const uint8_t request_golden[PROTOCOL_TEST_PACKET_SIZE] = {
        0xa5,0x5a,0x01,0x20,0x00,0x00,0x00,0x0e,
        0x01,0x23,0x45,0x67,0xa1,0xb2,0xc3,0xd4,
        0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80,
        0x7f,0xff,0x80,0x00,0xff,0x85,0x7f,0xbc,
        0x01,0xc8,0xfc,0xeb,0x80,0x00,
    };
    uint8_t frame[PROTOCOL_TEST_PACKET_SIZE];
    protocol_header_t header;
    imu_sample_t decoded;
    imu_sample_t response;

    protocol_encode_test_request(frame, &request, 0x01234567);
    assert_bytes(frame, request_golden, sizeof(frame));
    assert(protocol_decode_test_request(frame, sizeof(frame), &header, &decoded));
    assert(!protocol_decode_test_response(frame, sizeof(frame), &header, &decoded));
    assert(protocol_transform_test_sample(&decoded, 6, &response));
    assert(response.node_id == 6);
    assert(response.ax == INT16_MAX);
    assert(response.gx == INT16_MAX);
    assert(response.ay == INT16_MIN);
    assert(response.status ==
        (PROTOCOL_STATUS_MOCK_DATA | PROTOCOL_STATUS_TEST_REQUEST_PROCESSED));
    response.timestamp_us++;
    protocol_encode_test_response(frame, &response, 0x01234567);
    assert(frame[3] == PROTOCOL_MESSAGE_TYPE_TEST_RESPONSE);
    assert(protocol_decode_test_response(frame, sizeof(frame), &header, &decoded));
    assert(decoded.sequence == request.sequence && decoded.node_id == 6);
    assert(protocol_saturating_add_i16(INT16_MIN, -1) == INT16_MIN);
    assert(protocol_saturating_add_i16(-10, 3) == -7);

    frame[6] = 0; frame[7] = 15;
    assert(!protocol_decode_test_response(frame, sizeof(frame), &header, &decoded));
    assert(!protocol_decode_test_response(frame, sizeof(frame) - 1, &header, &decoded));
}

static void test_benchmark_payloads(void)
{
    const protocol_benchmark_control_t control = {
        .action = PROTOCOL_BENCHMARK_ACTION_START,
        .batch_frames = 100, .duration_ms = 60000,
    };
    const uint8_t control_golden[8] = {0x01,0x00,0x00,0x64,0x00,0x00,0xea,0x60};
    const protocol_benchmark_report_t report = {
        0x01020304, 0x11121314, 0x21222324,
        0x31323334, 0x41424344, 0x51525354,
    };
    const uint8_t report_golden[24] = {
        1,2,3,4, 0x11,0x12,0x13,0x14, 0x21,0x22,0x23,0x24,
        0x31,0x32,0x33,0x34, 0x41,0x42,0x43,0x44, 0x51,0x52,0x53,0x54,
    };
    uint8_t bytes[24];
    protocol_benchmark_control_t decoded_control;
    protocol_benchmark_report_t decoded_report;

    protocol_encode_benchmark_control_payload(bytes, &control);
    assert_bytes(bytes, control_golden, sizeof(control_golden));
    assert(protocol_decode_benchmark_control_payload(bytes, 8, &decoded_control));
    assert(decoded_control.batch_frames == 100 && decoded_control.duration_ms == 60000);
    bytes[1] = 1;
    assert(!protocol_decode_benchmark_control_payload(bytes, 8, &decoded_control));
    bytes[1] = 0; bytes[0] = 2;
    assert(!protocol_decode_benchmark_control_payload(bytes, 8, &decoded_control));
    assert(!protocol_decode_benchmark_control_payload(bytes, 7, &decoded_control));

    protocol_encode_benchmark_report_payload(bytes, &report);
    assert_bytes(bytes, report_golden, sizeof(report_golden));
    assert(protocol_decode_benchmark_report_payload(bytes, 24, &decoded_report));
    assert(decoded_report.send_failures == 0x51525354);
    assert(!protocol_decode_benchmark_report_payload(bytes, 23, &decoded_report));
}

static void fill_digest(uint8_t digest[PROTOCOL_SHA256_SIZE])
{
    size_t i;
    for (i = 0; i < PROTOCOL_SHA256_SIZE; ++i) digest[i] = (uint8_t)i;
}

static void test_file_payloads(void)
{
    protocol_file_begin_t begin = { .file_id = 0x01020304, .total_size = 0x05060708,
        .chunk_data_size = 900 };
    protocol_file_begin_t decoded_begin;
    protocol_file_end_t end = { .file_id = 0x11121314, .total_size = 0x21222324 };
    protocol_file_end_t decoded_end;
    protocol_file_result_t result = { .file_id = 0x31323334,
        .result_code = PROTOCOL_FILE_RESULT_CRC_MISMATCH,
        .bytes_received = 0x41424344 };
    protocol_file_result_t decoded_result;
    uint8_t result_packet[PROTOCOL_FILE_RESULT_PACKET_SIZE];
    const uint8_t chunk_data[] = {0xde,0xad,0xbe,0xef};
    protocol_file_chunk_t chunk = { .file_id = 0x01020304, .offset = 0x05060708,
        .data_length = sizeof(chunk_data), .crc32 = 0x7c9ca35a, .data = chunk_data };
    protocol_file_chunk_t decoded_chunk;
    uint8_t bytes[PROTOCOL_MAX_PAYLOAD_SIZE];
    size_t encoded_length;
    size_t i;

    fill_digest(begin.expected_sha256);
    protocol_encode_file_begin_payload(bytes, &begin);
    assert(bytes[0] == 1 && bytes[7] == 8 && bytes[8] == 0x03 && bytes[9] == 0x84);
    assert(bytes[10] == 0 && bytes[11] == 0);
    for (i = 0; i < 32; ++i) assert(bytes[12 + i] == i);
    assert(protocol_decode_file_begin_payload(bytes, 44, &decoded_begin));
    assert(decoded_begin.chunk_data_size == 900);
    bytes[11] = 1;
    assert(!protocol_decode_file_begin_payload(bytes, 44, &decoded_begin));
    bytes[11] = 0;
    assert(!protocol_decode_file_begin_payload(bytes, 43, &decoded_begin));

    assert(protocol_encode_file_chunk_payload(bytes, sizeof(bytes), &chunk, &encoded_length));
    {
        const uint8_t chunk_golden[20] = {
            1,2,3,4, 5,6,7,8, 0,4,0,0,
            0x7c,0x9c,0xa3,0x5a, 0xde,0xad,0xbe,0xef,
        };
        assert(encoded_length == sizeof(chunk_golden));
        assert_bytes(bytes, chunk_golden, sizeof(chunk_golden));
    }
    assert(protocol_decode_file_chunk_payload(bytes, encoded_length, &decoded_chunk));
    assert(decoded_chunk.data_length == 4 && decoded_chunk.data[3] == 0xef);
    bytes[11] = 1;
    assert(!protocol_decode_file_chunk_payload(bytes, encoded_length, &decoded_chunk));
    bytes[11] = 0; bytes[9] = 5;
    assert(!protocol_decode_file_chunk_payload(bytes, encoded_length, &decoded_chunk));
    bytes[9] = 4;
    assert(!protocol_decode_file_chunk_payload(bytes, 15, &decoded_chunk));
    assert(!protocol_encode_file_chunk_payload(bytes, 19, &chunk, &encoded_length));

    fill_digest(end.expected_sha256);
    protocol_encode_file_end_payload(bytes, &end);
    assert(bytes[0] == 0x11 && bytes[7] == 0x24 && bytes[8] == 0);
    assert(protocol_decode_file_end_payload(bytes, 40, &decoded_end));
    assert(decoded_end.total_size == end.total_size);
    assert(!protocol_decode_file_end_payload(bytes, 39, &decoded_end));

    fill_digest(result.calculated_sha256);
    protocol_encode_file_result_payload(bytes, &result);
    assert(bytes[4] == 0 && bytes[5] == 3 && bytes[6] == 0 && bytes[7] == 0);
    assert(protocol_decode_file_result_payload(bytes, 44, &decoded_result));
    assert(decoded_result.bytes_received == result.bytes_received);
    bytes[7] = 1;
    assert(!protocol_decode_file_result_payload(bytes, 44, &decoded_result));
    assert(!protocol_decode_file_result_payload(bytes, 45, &decoded_result));

    protocol_encode_file_result(result_packet, 3, 0x11223344, 0x55667788,
        UINT64_C(0x0102030405060708), &result);
    assert(result_packet[0] == 0xa5 && result_packet[1] == 0x5a);
    assert(result_packet[3] == PROTOCOL_MESSAGE_TYPE_FILE_RESULT);
    assert(result_packet[4] == 3 && result_packet[5] == 0);
    assert(result_packet[6] == 0 && result_packet[7] == 44);
    assert(result_packet[8] == 0x11 && result_packet[11] == 0x44);
    assert(result_packet[12] == 0x55 && result_packet[15] == 0x88);
    assert(protocol_decode_file_result_payload(
        result_packet + PROTOCOL_HEADER_SIZE,
        PROTOCOL_FILE_RESULT_PAYLOAD_SIZE, &decoded_result));
}

static void test_header_bounds(void)
{
    uint8_t bytes[PROTOCOL_HEADER_SIZE] = {0};
    protocol_header_t header;
    assert(!protocol_decode_header(bytes, PROTOCOL_HEADER_SIZE - 1, &header));
    assert(!protocol_decode_header(NULL, PROTOCOL_HEADER_SIZE, &header));
    assert(!protocol_decode_error_payload(bytes, 3, (protocol_error_t *)&header));
}

static void test_error_golden_vector(void)
{
    const uint8_t expected[PROTOCOL_ERROR_PACKET_SIZE] = {
        0xa5,0x5a,0x01,0x7f,0x03,0x00,0x00,0x04,
        0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x00,0x08,0x20,0x00,
    };
    uint8_t packet[PROTOCOL_ERROR_PACKET_SIZE];
    protocol_error_t decoded;

    protocol_encode_error(packet, 3, 0x11223344, 0x55667788,
        UINT64_C(0x0102030405060708), PROTOCOL_ERROR_SEQUENCE_REUSE,
        PROTOCOL_MESSAGE_TYPE_TEST_REQUEST);
    assert_bytes(packet, expected, sizeof(expected));
    assert(protocol_decode_error_payload(packet + PROTOCOL_HEADER_SIZE,
        PROTOCOL_ERROR_PAYLOAD_SIZE, &decoded));
    assert(decoded.error_code == PROTOCOL_ERROR_SEQUENCE_REUSE);
    assert(decoded.offending_message_type == PROTOCOL_MESSAGE_TYPE_TEST_REQUEST);
}

static void test_sync_and_start_payloads(void)
{
    uint8_t request[PROTOCOL_SYNC_REQ_PAYLOAD_SIZE] = {
        1,2,3,4,5,6,7,8,
    };
    uint64_t coordinator_send_us = 0;
    protocol_sync_response_t response = {
        UINT64_C(0x0102030405060708),
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728),
    };
    uint8_t packet[PROTOCOL_SYNC_RESP_PACKET_SIZE];
    protocol_header_t header;
    uint8_t start_payload[PROTOCOL_START_PAYLOAD_SIZE] = {
        0,0,0,0,0,0,0x27,0x10,
        0xff,0xff,0xff,0xff,0xff,0xff,0xfc,0x18,
    };
    protocol_start_t start;
    const uint8_t arm_payload[PROTOCOL_ARM_PAYLOAD_SIZE] = {
        0,0,0,0,0,0,0x4e,0x20,
        0xff,0xff,0xff,0xff,0xff,0xff,0xfc,0x18,
        0x11,0x12,0x13,0x14,
    };
    protocol_arm_t arm;

    assert(protocol_decode_sync_request_payload(request, sizeof(request),
        &coordinator_send_us));
    assert(coordinator_send_us == UINT64_C(0x0102030405060708));
    assert(!protocol_decode_sync_request_payload(request, 7,
        &coordinator_send_us));

    protocol_encode_sync_response(packet, 2, 0x11223344, 0x55667788,
        &response);
    assert(protocol_decode_header(packet, sizeof(packet), &header));
    assert(header.message_type == PROTOCOL_MESSAGE_TYPE_SYNC_RESP);
    assert(header.node_id == 2 && header.session_id == 0x11223344);
    assert(header.sequence == 0x55667788);
    assert(header.timestamp_us == response.node_send_us);
    assert_bytes(packet + PROTOCOL_HEADER_SIZE, request, sizeof(request));
    assert(packet[32] == 0x11 && packet[39] == 0x18);
    assert(packet[40] == 0x21 && packet[47] == 0x28);

    assert(protocol_decode_start_payload(start_payload, sizeof(start_payload),
        &start));
    assert(start.coordinator_start_us == 10000);
    assert(start.node_minus_coordinator_us == -1000);
    assert(!protocol_decode_start_payload(start_payload, 15, &start));

    assert(protocol_decode_arm_payload(arm_payload, sizeof(arm_payload), &arm));
    assert(arm.coordinator_arm_us == 20000);
    assert(arm.node_minus_coordinator_us == -1000);
    assert(arm.arm_generation == 0x11121314);
    assert(!protocol_decode_arm_payload(arm_payload, sizeof(arm_payload) - 1,
        &arm));
}

static void test_control_ack_layout(void)
{
    protocol_control_ack_t ack = {
        .command_type = PROTOCOL_MESSAGE_TYPE_START,
        .result = PROTOCOL_CONTROL_RESULT_APPLIED,
        .command_sequence = 0x01020304,
        .effective_local_us = UINT64_C(0x1112131415161718),
    };
    uint8_t packet[PROTOCOL_CONTROL_ACK_PACKET_SIZE];
    protocol_header_t header;
    protocol_encode_control_ack(packet, 2, 0x21222324, 0x31323334,
        UINT64_C(0x4142434445464748), &ack);
    assert(protocol_decode_header(packet, sizeof(packet), &header));
    assert(header.message_type == PROTOCOL_MESSAGE_TYPE_CONTROL_ACK);
    assert(header.payload_length == PROTOCOL_CONTROL_ACK_PAYLOAD_SIZE);
    assert(packet[24] == PROTOCOL_MESSAGE_TYPE_START && packet[25] == 0);
    assert(packet[26] == 0 && packet[27] == 0);
    assert(packet[28] == 1 && packet[31] == 4);
    assert(packet[32] == 0x11 && packet[39] == 0x18);
}

static void test_physical_data_golden_vector(void)
{
    protocol_physical_sample_t input = {
        .node_id = 1, .flags = 0x02, .sequence = 0x01020304,
        .timestamp_us = UINT64_C(0x0102030405060708),
        .ax = 1.0f, .ay = -2.0f, .az = 0.5f,
        .gx = -0.25f, .gy = 3.0f, .gz = 0.0f,
        .status = 0x0003,
    };
    const uint8_t expected[PROTOCOL_PHYSICAL_DATA_PACKET_SIZE] = {
        0xa5,0x5a,0x01,0x12,0x01,0x02,0x00,0x1a,
        0x11,0x22,0x33,0x44,0x01,0x02,0x03,0x04,
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x3f,0x80,0x00,0x00,0xc0,0x00,0x00,0x00,
        0x3f,0x00,0x00,0x00,0xbe,0x80,0x00,0x00,
        0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x03,
    };
    uint8_t frame[PROTOCOL_PHYSICAL_DATA_PACKET_SIZE];
    protocol_header_t header;
    protocol_physical_sample_t decoded;
    assert(protocol_encode_physical_data(frame, &input, 0x11223344));
    assert_bytes(frame, expected, sizeof(expected));
    assert(protocol_decode_physical_data(frame, sizeof(frame), &header, &decoded));
    assert(decoded.ax == 1.0f && decoded.gy == 3.0f && decoded.status == 3);
    input.ax = NAN;
    assert(!protocol_encode_physical_data(frame, &input, 0x11223344));
    memcpy(frame, expected, sizeof(frame));
    frame[24] = 0x7f; frame[25] = 0x80; frame[26] = 0; frame[27] = 0;
    assert(!protocol_decode_physical_data(frame, sizeof(frame), &header, &decoded));
    assert(!protocol_decode_physical_data(frame, sizeof(frame) - 1, &header, &decoded));
}

static void test_trigger_and_flush_golden_vectors(void)
{
    protocol_activity_trigger_t trigger = {
        .boot_id = 0x01020304, .arm_generation = 0x11121314,
        .peak_timestamp_us = UINT64_C(0x2122232425262728),
    };
    const uint8_t expected[PROTOCOL_ACTIVITY_TRIGGER_PACKET_SIZE] = {
        0xa5,0x5a,0x01,0x05,0x02,0x00,0x00,0x10,
        0x31,0x32,0x33,0x34,0x41,0x42,0x43,0x44,
        0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,
        0x01,0x02,0x03,0x04,0x11,0x12,0x13,0x14,
        0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
    };
    uint8_t frame[PROTOCOL_ACTIVITY_TRIGGER_PACKET_SIZE];
    protocol_header_t header;
    protocol_activity_trigger_t decoded_trigger;
    protocol_encode_activity_trigger(frame, 2, 0x31323334, 0x41424344,
        UINT64_C(0x5152535455565758), &trigger);
    assert_bytes(frame, expected, sizeof(expected));
    assert(protocol_decode_activity_trigger(frame, sizeof(frame), &header,
        &decoded_trigger));
    assert(decoded_trigger.boot_id == trigger.boot_id);

    const uint8_t payload[PROTOCOL_START_FLUSH_PAYLOAD_SIZE] = {
        0,0,0,0,0,0,0x27,0x10,
        0xff,0xff,0xff,0xff,0xff,0xff,0xfc,0x18,
        0x00,0x4c,0x4b,0x40, 0x02,0,0,0,
        0x41,0x42,0x43,0x44,
    };
    protocol_start_flush_t flush;
    assert(protocol_decode_start_flush_payload(payload, sizeof(payload), &flush));
    assert(flush.coordinator_t0_us == 10000);
    assert(flush.node_minus_coordinator_us == -1000);
    assert(flush.history_duration_us == 5000000);
    assert(flush.trigger_node_id == 2 && flush.trigger_sequence == 0x41424344);
    assert(!protocol_decode_start_flush_payload(payload, sizeof(payload) - 1, &flush));
}

int main(void)
{
    test_existing_data_golden_vector();
    test_test_messages_and_transform();
    test_benchmark_payloads();
    test_file_payloads();
    test_header_bounds();
    test_error_golden_vector();
    test_sync_and_start_payloads();
    test_control_ack_layout();
    test_physical_data_golden_vector();
    test_trigger_and_flush_golden_vectors();
    puts("protocol host tests: PASS");
    return 0;
}
