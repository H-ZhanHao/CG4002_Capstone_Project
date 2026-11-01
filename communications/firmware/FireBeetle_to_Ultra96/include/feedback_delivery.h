#pragma once
#include "protocol.h"

#define FEEDBACK_DELIVERY_CACHE_SIZE 32
typedef struct {
    bool valid;
    uint32_t sequence;
    protocol_feedback_t request;
    protocol_control_ack_t ack;
} feedback_delivery_entry_t;
typedef struct {
    uint32_t session_id;
    uint8_t node_id;
    bool seen_sequence;
    uint32_t highest_sequence;
    uint64_t highest_event;
    size_t next_entry;
    feedback_delivery_entry_t entries[FEEDBACK_DELIVERY_CACHE_SIZE];
} feedback_delivery_t;
typedef bool (*feedback_apply_fn)(uint8_t action, uint64_t now_us, void *context);

// Pipeline-owned state, survives STOP/ARM/START and resets only on Session change.
void feedback_delivery_begin(feedback_delivery_t *state, uint32_t session, uint8_t node);
protocol_control_ack_t feedback_delivery_process(feedback_delivery_t *state,
    uint32_t session, uint32_t sequence, const protocol_feedback_t *request,
    uint64_t now_us, feedback_apply_fn apply, void *context);
