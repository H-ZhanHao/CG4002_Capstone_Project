#include "protocol.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const uint8_t on[] = {1, 0, 0, 0}, off[] = {0, 0, 0, 0};
    const uint8_t bad_value[] = {2, 0, 0, 0}, bad_reserved[] = {1, 0, 1, 0};
    bool enabled;
    assert(protocol_decode_fall_detection_payload(on, sizeof(on), &enabled) && enabled);
    assert(protocol_decode_fall_detection_payload(off, sizeof(off), &enabled) && !enabled);
    assert(!protocol_decode_fall_detection_payload(bad_value, sizeof(bad_value), &enabled));
    assert(!protocol_decode_fall_detection_payload(bad_reserved, sizeof(bad_reserved), &enabled));
    assert(!protocol_decode_fall_detection_payload(on, sizeof(on) - 1, &enabled));
    assert(!protocol_decode_fall_detection_payload(NULL, sizeof(on), &enabled));
    assert(!protocol_decode_fall_detection_payload(on, sizeof(on), NULL));
    uint8_t frame[PROTOCOL_CONTROL_ACK_PACKET_SIZE];
    const protocol_control_ack_t ack = {
        .command_type = PROTOCOL_MESSAGE_TYPE_FALL_DETECTION_SET,
        .result = PROTOCOL_CONTROL_RESULT_APPLIED, .command_sequence = 42,
        .effective_local_us = 1000,
    };
    protocol_encode_control_ack(frame, 1, 101, 42, 1000, &ack);
    assert(frame[3] == PROTOCOL_MESSAGE_TYPE_CONTROL_ACK && frame[24] == 0x0c);
    assert(frame[25] == 0 && frame[31] == 42);
    puts("Fall setting protocol: strict boolean/reserved bytes and existing ACK layout passed");
    return 0;
}
