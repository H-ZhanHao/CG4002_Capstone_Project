#include "sensor_history.h"
#include <assert.h>
#include <stdio.h>

static protocol_physical_sample_t sample(uint32_t sequence, uint64_t timestamp)
{
    protocol_physical_sample_t value = {0};
    value.node_id = 1;
    value.sequence = sequence;
    value.timestamp_us = timestamp;
    value.az = 9.80665f;
    return value;
}

static void test_live_join_without_pre_peak_history(void)
{
    physical_history_t history;
    physical_feeder_t feeder = {0};
    protocol_physical_sample_t output;
    physical_history_clear(&history);
    assert(!physical_feeder_begin_live(&feeder, &history, 20, 0, 500));
    protocol_physical_sample_t first = sample(0, 500);
    assert(physical_history_push(&history, &first));
    assert(physical_feeder_begin_live(&feeder, &history, 20, 0, 600));
    assert(!physical_feeder_begin_live(&feeder, &history, 21, 0, 600));
    assert(!physical_feeder_next(&feeder, &history, &output));
    protocol_physical_sample_t next = sample(1, 10500);
    assert(physical_history_push(&history, &next));
    assert(physical_feeder_next(&feeder, &history, &output));
    assert(output.sequence == 1);
    assert(!(output.status & PROTOCOL_STATUS_HISTORICAL));
    assert(physical_feeder_retry(&feeder, 1));
    assert(physical_feeder_next(&feeder, &history, &output));
    assert(physical_feeder_mark_sent(&feeder, 1));
    assert(!physical_feeder_next(&feeder, &history, &output));
    physical_feeder_stop(&feeder);
    assert(!feeder.active);
}

int main(void)
{
    test_live_join_without_pre_peak_history();
    puts("live history tests: PASS");
    return 0;
}
