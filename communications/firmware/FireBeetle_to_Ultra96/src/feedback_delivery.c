#include "feedback_delivery.h"
#include <string.h>

void feedback_delivery_begin(feedback_delivery_t *state, uint32_t session, uint8_t node)
{
    memset(state, 0, sizeof(*state));
    state->session_id = session;
    state->node_id = node;
}

static bool same_request(const protocol_feedback_t *a, const protocol_feedback_t *b)
{
    return a->event_id == b->event_id && a->action == b->action;
}

protocol_control_ack_t feedback_delivery_process(feedback_delivery_t *state,
    uint32_t session, uint32_t sequence, const protocol_feedback_t *request,
    uint64_t now_us, feedback_apply_fn apply, void *context)
{
    protocol_control_ack_t ack = { .command_type = PROTOCOL_MESSAGE_TYPE_FEEDBACK_COMMAND,
        .command_sequence = sequence, .effective_local_us = now_us,
        .result = PROTOCOL_CONTROL_RESULT_STALE_IDENTITY };
    if (session == 0 || session != state->session_id) return ack;
    ack.result = PROTOCOL_CONTROL_RESULT_INVALID_STATE;
    if (request == NULL || request->event_id == 0) return ack;
    // Remember multiple intervening commands; identical transport retries cannot
    // restart outputs. Comparing fields avoids padding-dependent identity checks.
    const feedback_delivery_entry_t *logical = NULL;
    for (size_t i = 0; i < FEEDBACK_DELIVERY_CACHE_SIZE; ++i) {
        const feedback_delivery_entry_t *entry = &state->entries[i];
        if (!entry->valid) continue;
        if (entry->sequence == sequence) {
            if (!same_request(&entry->request, request)) {
                ack.result = PROTOCOL_CONTROL_RESULT_CONFLICT;
                return ack;
            }
            ack = entry->ack;
            if (ack.result == PROTOCOL_CONTROL_RESULT_APPLIED)
                ack.result = PROTOCOL_CONTROL_RESULT_DUPLICATE;
            return ack;
        }
        if (entry->request.event_id == request->event_id) logical = entry;
    }
    if (state->seen_sequence && sequence <= state->highest_sequence) {
        ack.result = PROTOCOL_CONTROL_RESULT_STALE_IDENTITY;
        return ack;
    }
    if (logical != NULL) {
        if (!same_request(&logical->request, request)) {
            ack.result = PROTOCOL_CONTROL_RESULT_CONFLICT;
            return ack;
        }
        ack = logical->ack;
        ack.command_sequence = sequence;
        if (ack.result == PROTOCOL_CONTROL_RESULT_APPLIED)
            ack.result = PROTOCOL_CONTROL_RESULT_DUPLICATE;
    } else if (request->event_id <= state->highest_event) {
        // Eviction never makes an old event eligible for replay.
        ack.result = PROTOCOL_CONTROL_RESULT_STALE_IDENTITY;
        return ack;
    } else {
        if (request->action < PROTOCOL_FEEDBACK_REP_BUZZ ||
            request->action > PROTOCOL_FEEDBACK_ACTIVITY_IDLE ||
            (request->action == PROTOCOL_FEEDBACK_REP_BUZZ && state->node_id != 1))
            ack.result = PROTOCOL_CONTROL_RESULT_UNSUPPORTED;
        else
            ack.result = apply != NULL && apply(request->action, now_us, context)
                ? PROTOCOL_CONTROL_RESULT_APPLIED : PROTOCOL_CONTROL_RESULT_BUSY;
    }
    state->seen_sequence = true;
    state->highest_sequence = sequence;
    if (request->event_id > state->highest_event) state->highest_event = request->event_id;
    feedback_delivery_entry_t *entry = &state->entries[state->next_entry];
    entry->valid = true;
    entry->sequence = sequence;
    entry->request = *request;
    entry->ack = ack;
    // Store APPLIED for logically duplicated accepted requests too.
    if (entry->ack.result == PROTOCOL_CONTROL_RESULT_DUPLICATE)
        entry->ack.result = PROTOCOL_CONTROL_RESULT_APPLIED;
    state->next_entry = (state->next_entry + 1) % FEEDBACK_DELIVERY_CACHE_SIZE;
    return ack;
}
