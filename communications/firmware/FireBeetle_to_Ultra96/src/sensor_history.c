#include "sensor_history.h"

#include <math.h>
#include <string.h>

static const protocol_physical_sample_t *at(
    const physical_history_t *history, size_t index)
{
    return &history->samples[(history->head + index) % PHYSICAL_HISTORY_CAPACITY];
}

void physical_history_clear(physical_history_t *history)
{
    memset(history, 0, sizeof(*history));
}

bool physical_history_push(physical_history_t *history,
                           const protocol_physical_sample_t *sample)
{
    if (history == NULL || sample == NULL ||
        !isfinite(sample->ax) || !isfinite(sample->ay) ||
        !isfinite(sample->az) || !isfinite(sample->gx) ||
        !isfinite(sample->gy) || !isfinite(sample->gz)) return false;
    if (history->count != 0) {
        const protocol_physical_sample_t *newest = at(history, history->count - 1);
        if (sample->timestamp_us <= newest->timestamp_us ||
            sample->sequence != newest->sequence + 1U) return false;
    }
    while (history->count != 0 &&
           sample->timestamp_us - at(history, 0)->timestamp_us >
               PHYSICAL_HISTORY_RETENTION_US) {
        history->head = (history->head + 1U) % PHYSICAL_HISTORY_CAPACITY;
        history->count--;
        history->overwrite_count++;
    }
    if (history->count == PHYSICAL_HISTORY_CAPACITY) {
        history->head = (history->head + 1U) % PHYSICAL_HISTORY_CAPACITY;
        history->count--;
        history->overwrite_count++;
    }
    history->samples[(history->head + history->count) %
        PHYSICAL_HISTORY_CAPACITY] = *sample;
    history->count++;
    return true;
}

bool physical_feeder_begin(physical_feeder_t *feeder,
                           const physical_history_t *history,
                           uint32_t command_sequence,
                           uint32_t trigger_sequence,
                           uint64_t history_start_us,
                           uint64_t t0_us)
{
    if (feeder == NULL || history == NULL || history->count == 0 ||
        history_start_us >= t0_us || at(history, 0)->timestamp_us > history_start_us ||
        at(history, history->count - 1)->timestamp_us < t0_us) return false;
    memset(feeder, 0, sizeof(*feeder));
    feeder->active = true;
    feeder->command_sequence = command_sequence;
    feeder->trigger_sequence = trigger_sequence;
    feeder->history_start_us = history_start_us;
    feeder->t0_us = t0_us;
    for (size_t i = 0; i < history->count; ++i) {
        const protocol_physical_sample_t *candidate = at(history, i);
        if (candidate->timestamp_us >= history_start_us) {
            feeder->next_sequence = candidate->sequence;
            return true;
        }
    }
    memset(feeder, 0, sizeof(*feeder));
    return false;
}

void physical_feeder_stop(physical_feeder_t *feeder)
{
    memset(feeder, 0, sizeof(*feeder));
}

bool physical_feeder_next(physical_feeder_t *feeder,
                          const physical_history_t *history,
                          protocol_physical_sample_t *sample)
{
    if (feeder == NULL || history == NULL || sample == NULL ||
        !feeder->active || feeder->awaiting_mark || history->count == 0) return false;
    const protocol_physical_sample_t *oldest = at(history, 0);
    if (oldest->sequence > feeder->next_sequence) {
        feeder->lost_before_send += oldest->sequence - feeder->next_sequence;
        feeder->next_sequence = oldest->sequence;
    }
    for (size_t i = 0; i < history->count; ++i) {
        const protocol_physical_sample_t *candidate = at(history, i);
        if (candidate->sequence != feeder->next_sequence) continue;
        *sample = *candidate;
        sample->status |= PROTOCOL_STATUS_SENSOR_VALID;
        if (candidate->timestamp_us <= feeder->t0_us)
            sample->status |= PROTOCOL_STATUS_HISTORICAL;
        else
            sample->status &= (uint16_t)~PROTOCOL_STATUS_HISTORICAL;
        /* Normal age-based eviction from the rolling ring is not data loss.
         * Report only samples that expired before an active feeder sent them. */
        if (feeder->lost_before_send != 0)
            sample->status |= PROTOCOL_STATUS_HISTORY_OVERWRITE;
        feeder->offered_sequence = candidate->sequence;
        feeder->awaiting_mark = true;
        return true;
    }
    return false;
}

bool physical_feeder_mark_sent(physical_feeder_t *feeder, uint32_t sequence)
{
    if (feeder == NULL || !feeder->active || !feeder->awaiting_mark ||
        sequence != feeder->offered_sequence) return false;
    feeder->next_sequence = sequence + 1U;
    feeder->awaiting_mark = false;
    return true;
}

bool physical_feeder_retry(physical_feeder_t *feeder, uint32_t sequence)
{
    if (feeder == NULL || !feeder->active || !feeder->awaiting_mark ||
        sequence != feeder->offered_sequence) return false;
    feeder->awaiting_mark = false;
    return true;
}
