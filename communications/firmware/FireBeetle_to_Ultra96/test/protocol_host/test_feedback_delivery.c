#include "feedback_delivery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned outputs;
static bool accept = true;
static bool apply(uint8_t action, uint64_t now_us, void *context)
{
    (void)context;
    assert(action >= 4 && action <= 6 && now_us != 0);
    if (accept) ++outputs;
    return accept;
}

static void codecs(void)
{
    const uint8_t expected[16] = {
        0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef, 5,0,0,0,0,0,0,0
    };
    uint8_t payload[16];
    protocol_feedback_t request = {.event_id=0x0123456789abcdefULL, .action=5}, decoded;
    assert(protocol_encode_feedback_payload(payload, &request));
    assert(memcmp(payload, expected, sizeof(payload)) == 0);
    assert(protocol_decode_feedback_payload(expected, sizeof(expected), &decoded));
    assert(decoded.event_id == request.event_id && decoded.action == request.action);
    assert(!protocol_decode_feedback_payload(expected, 15, &decoded));
    for (size_t i=9; i<16; ++i) {
        memcpy(payload, expected, 16); payload[i]=1;
        assert(!protocol_decode_feedback_payload(payload, 16, &decoded));
    }
    memcpy(payload, expected, 16); payload[8]=1;
    assert(!protocol_decode_feedback_payload(payload, 16, &decoded));
    payload[8]=0;
    assert(!protocol_decode_feedback_payload(payload, 16, &decoded));
    memset(payload, 0, 16); payload[8]=1;
    assert(!protocol_decode_feedback_payload(payload, 16, &decoded));
    request.event_id=0;
    assert(!protocol_encode_feedback_payload(payload, &request));
    protocol_control_ack_t ack = {.command_type=0x0b, .result=3,
        .command_sequence=0x05060708, .effective_local_us=0x0102030405060708ULL};
    uint8_t packet[40];
    const uint8_t ack_expected[40] = {
        0xa5,0x5a,1,9,2,0,0,16, 1,2,3,4, 5,6,7,8,
        0,0,0,0,0,0,0,9, 0x0b,3,0,0, 5,6,7,8, 1,2,3,4,5,6,7,8
    };
    protocol_encode_control_ack(packet,2,0x01020304,0x05060708,9,&ack);
    assert(memcmp(packet,ack_expected,40)==0);
}

int main(void)
{
    codecs();
    feedback_delivery_t state;
    feedback_delivery_begin(&state,42,1);
    protocol_feedback_t request={.event_id=1,.action=4};
    protocol_control_ack_t ack;
#define PROCESS(session, seq) feedback_delivery_process(&state,session,seq,&request,1000+(seq),apply,NULL)
    ack=PROCESS(41,1); assert(ack.result==PROTOCOL_CONTROL_RESULT_STALE_IDENTITY && outputs==0);
    ack=PROCESS(42,1); assert(ack.result==0 && outputs==1);
    uint64_t applied_at=ack.effective_local_us;
    ack=PROCESS(42,1); assert(ack.result==3 && outputs==1 && ack.effective_local_us==applied_at);
    request.event_id=2;
    ack=PROCESS(42,2); assert(ack.result==0 && outputs==2);
    request.event_id=1;
    ack=PROCESS(42,1); assert(ack.result==3 && outputs==2); // Delayed retry after another command.
    ack=PROCESS(42,3); assert(ack.result==3 && outputs==2); // Same event, new transport sequence.
    request.action=5;
    ack=PROCESS(42,4); assert(ack.result==8 && outputs==2); // Logical identity conflict.
    request.event_id=3; request.action=4;
    ack=PROCESS(42,2); assert(ack.result==8 && outputs==2); // Transport identity conflict.
    for (unsigned i=10;i<60;++i) {
        request.event_id=i;
        ack=PROCESS(42,i); assert(ack.result==0);
    }
    unsigned count=outputs;
    request.event_id=1;
    ack=PROCESS(42,1); assert(ack.result==5 && outputs==count);
    ack=PROCESS(42,100); assert(ack.result==5 && outputs==count); // Eviction never enables replay.
    request.event_id=100; request.action=5; accept=false;
    ack=PROCESS(42,100); assert(ack.result==6);
    accept=true;
    ack=PROCESS(42,100); assert(ack.result==6 && outputs==count);
    feedback_delivery_begin(&state,43,2);
    request.event_id=1; request.action=4;
    ack=PROCESS(43,1); assert(ack.result==7 && outputs==count); // Wrist buzzer unsupported.
    ack=PROCESS(43,1); assert(ack.result==7 && outputs==count);
    request.event_id=2; request.action=6;
    ack=PROCESS(42,2); assert(ack.result==5 && outputs==count);
    ack=PROCESS(43,2); assert(ack.result==0 && outputs==count+1);
    puts("Feedback codec, ACK bytes, sessions, delayed retries, conflicts, eviction and unsupported output: PASS");
    return 0;
}
