/* Desktop peer uses the exact production queue and codecs, controlled by stdin. */
#include "device_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    device_events_t state; device_events_init(&state, 0x01020304);
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned long long now; unsigned value;
        if (sscanf(line, "push %llu %u", &now, &value)==2) {
            printf("%d\n", device_events_push(&state, now, value!=0));
        } else if (sscanf(line, "due %llu %u", &now, &value)==2) {
            protocol_device_event_t event; uint8_t frame[PROTOCOL_DEVICE_EVENT_PACKET_SIZE];
            if (device_events_due(&state, now, &event) && protocol_encode_device_event(frame, value, &event)) {
                for (unsigned i=0; i<sizeof(frame); ++i) printf("%02x", frame[i]);
                puts("");
            } else puts("-");
        } else if (!strncmp(line, "ack ", 4)) {
            uint8_t frame[PROTOCOL_DEVICE_EVENT_ACK_PACKET_SIZE]; bool valid=true;
            for (unsigned i=0; i<sizeof(frame); ++i) {
                unsigned byte;
                if (sscanf(line+4+2*i, "%2x", &byte)!=1) { valid=false; break; }
                frame[i]=(uint8_t)byte;
            }
            protocol_device_event_ack_t ack;
            printf("%d\n", valid && protocol_decode_device_event_ack(frame, sizeof(frame), &ack) && device_events_ack(&state, &ack));
        } else puts("invalid");
        fflush(stdout);
    }
    return 0;
}
