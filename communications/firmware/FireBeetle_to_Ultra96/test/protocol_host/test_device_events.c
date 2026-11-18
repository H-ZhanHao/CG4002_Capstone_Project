#include "device_events.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    device_events_t state; device_events_init(&state, 0x01020304);
    protocol_device_event_t event, decoded;
    assert(device_events_push(&state, 100, true));
    assert(device_events_due(&state, 100, &event));
    assert(event.event_id == 1 && event.synthetic == 1 && event.duration_ms == 3000);
    assert(!device_events_due(&state, 101, &decoded));
    assert(device_events_due(&state, 100+DEVICE_EVENT_RETRY_US, &decoded));
    assert(decoded.event_id == event.event_id && decoded.occurred_us == 100);
    protocol_device_event_ack_t ack = {.boot_id=42, .event_id=1};
    assert(!device_events_ack(&state, &ack));
    ack.boot_id = state.boot_id; ack.result=2;
    assert(!device_events_ack(&state, &ack));
    ack.result=1; assert(device_events_ack(&state, &ack));
    assert(!device_events_ack(&state, &ack));
    for (unsigned i=0; i<DEVICE_EVENTS_CAPACITY; ++i) assert(device_events_push(&state, 1000000, false));
    assert(!device_events_push(&state, 1000000, true)); assert(state.overflow == 1);
    device_events_expire(&state, 1000000+DEVICE_EVENT_TTL_US);
    assert(state.expired == DEVICE_EVENTS_CAPACITY);
    assert(!device_events_due(&state, 1000000+DEVICE_EVENT_TTL_US, &decoded));
    assert(device_events_push(&state, 40000000, false));
    assert(device_events_due(&state, 40000000, &event));
    uint8_t frame[PROTOCOL_DEVICE_EVENT_PACKET_SIZE];
    assert(protocol_encode_device_event(frame, 0x10203040, &event));
    assert(frame[0]==0xa5 && frame[1]==0x5a && frame[3]==0x13 && frame[4]==1 && frame[7]==32);
    assert(protocol_decode_device_event(frame, sizeof(frame), &decoded));
    assert(decoded.event_id == event.event_id && decoded.boot_id == state.boot_id);
    for (unsigned i=0; i<sizeof(frame); ++i) {
        if (i==8 || i==9 || i==10 || i==11 || (i>=28 && i<=31)) continue;
        // Full malformed-field coverage is also exercised from Python.
        if (i==5 || i==26 || i==27 || i>=52) {
            frame[i]=1; assert(!protocol_decode_device_event(frame, sizeof(frame), &decoded)); frame[i]=0;
        }
    }
    assert(!protocol_decode_device_event(frame, sizeof(frame)-1, &decoded));
    // Capacity reuse must not make event identities go backwards.
    assert(event.event_id == DEVICE_EVENTS_CAPACITY+2);
    device_events_init(&state, 99);
    assert(device_events_push(&state, 1000000, false));
    ack = (protocol_device_event_ack_t){ .boot_id=99, .event_id=1 };
    assert(!device_events_ack(&state, &ack)); // Cannot ACK an unsent event.
    for (unsigned i=0; i<DEVICE_EVENT_MAX_ATTEMPTS; ++i)
        assert(device_events_due(&state, 1000000+i*DEVICE_EVENT_RETRY_US, &event));
    assert(!device_events_due(&state, 1000000+DEVICE_EVENT_TTL_US, &event));
    assert(state.expired==1);
    puts("Device event queue/codecs: retries, identity, ACK, bounds, overflow and expiry PASS");
    return 0;
}
