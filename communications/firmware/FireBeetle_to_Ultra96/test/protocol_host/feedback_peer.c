/* Desktop wire peer for Python end-to-end tests. Uses production C codecs and
 * delivery engine. Each input line is one complete hex-encoded command frame;
 * each response is CONTROL_ACK hex plus a count of actual apply callbacks. */
#include "feedback_delivery.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned outputs;
static bool output(uint8_t action, uint64_t now_us, void *context)
{
    (void)action; (void)now_us; (void)context;
    ++outputs;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    uint8_t node=(uint8_t)strtoul(argv[1],NULL,10);
    uint32_t session=(uint32_t)strtoul(argv[2],NULL,10);
    feedback_delivery_t state;
    feedback_delivery_begin(&state,session,node);
    char line[256];
    while (fgets(line,sizeof(line),stdin)) {
        uint8_t frame[40];
        unsigned value;
        size_t count=0;
        while (count<sizeof(frame) && sscanf(line+count*2,"%2x",&value)==1) {
            frame[count]=(uint8_t)value; ++count;
        }
        protocol_header_t header;
        protocol_feedback_t request;
        if (!protocol_decode_header(frame,count,&header)) return 3;
        protocol_control_ack_t ack={.command_type=0x0b,.result=2,
            .command_sequence=header.sequence,.effective_local_us=1000000};
        if (count==40 && header.magic==PROTOCOL_MAGIC && header.version==1 &&
            header.node_id==0 && header.flags==0 && header.message_type==0x0b &&
            header.payload_length==16 && protocol_decode_feedback_payload(frame+24,16,&request))
            ack=feedback_delivery_process(&state,header.session_id,header.sequence,
                &request,1000000,output,NULL);
        uint8_t packet[40];
        protocol_encode_control_ack(packet,node,session,header.sequence,1000000,&ack);
        for (size_t i=0;i<sizeof(packet);++i) printf("%02x",packet[i]);
        printf(" %u\n",outputs);
        fflush(stdout);
    }
    return 0;
}
