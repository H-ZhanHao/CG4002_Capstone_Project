#ifndef SENSOR_HISTORY_H
#define SENSOR_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#define PHYSICAL_SAMPLE_RATE_HZ 50U
#define PHYSICAL_SAMPLE_PERIOD_US 20000U
#define PHYSICAL_HISTORY_RETENTION_US UINT64_C(8000000)
#define PHYSICAL_HISTORY_CAPACITY 401U

typedef struct {
    protocol_physical_sample_t samples[PHYSICAL_HISTORY_CAPACITY];
    size_t head;
    size_t count;
    uint32_t overwrite_count;
} physical_history_t;

typedef struct {
    bool active;
    bool awaiting_mark;
    uint32_t command_sequence;
    uint32_t trigger_sequence;
    uint64_t history_start_us;
    uint64_t t0_us;
    uint32_t next_sequence;
    uint32_t offered_sequence;
    uint32_t lost_before_send;
} physical_feeder_t;

void physical_history_clear(physical_history_t *history);
bool physical_history_push(physical_history_t *history,
                           const protocol_physical_sample_t *sample);
bool physical_feeder_begin(physical_feeder_t *feeder,
                           const physical_history_t *history,
                           uint32_t command_sequence,
                           uint32_t trigger_sequence,
                           uint64_t history_start_us,
                           uint64_t t0_us);
void physical_feeder_stop(physical_feeder_t *feeder);
bool physical_feeder_next(physical_feeder_t *feeder,
                          const physical_history_t *history,
                          protocol_physical_sample_t *sample);
bool physical_feeder_mark_sent(physical_feeder_t *feeder, uint32_t sequence);
bool physical_feeder_retry(physical_feeder_t *feeder, uint32_t sequence);

#endif
