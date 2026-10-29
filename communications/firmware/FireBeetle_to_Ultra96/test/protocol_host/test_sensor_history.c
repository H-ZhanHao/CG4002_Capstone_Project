#include "sensor_history.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static protocol_physical_sample_t sample(uint32_t sequence, uint64_t timestamp_us)
{
    protocol_physical_sample_t result = {
        .node_id = 1, .sequence = sequence, .timestamp_us = timestamp_us,
        .ax = 0.0f, .ay = 0.0f, .az = 9.80665f,
        .gx = 0.0f, .gy = 0.0f, .gz = 0.0f,
    };
    return result;
}

static void test_history_to_live_exactly_once(void)
{
    physical_history_t history;
    physical_feeder_t feeder = {0};
    physical_history_clear(&history);
    for (uint32_t sequence = 0; sequence <= 300; ++sequence) {
        protocol_physical_sample_t value = sample(sequence,
            UINT64_C(1000000) + (uint64_t)sequence * PHYSICAL_SAMPLE_PERIOD_US);
        assert(physical_history_push(&history, &value));
    }
    const uint64_t t0 = UINT64_C(7000000);
    assert(physical_feeder_begin(&feeder, &history, 9, 7,
        t0 - UINT64_C(5000000), t0));
    uint32_t expected = 50;
    protocol_physical_sample_t value;
    while (physical_feeder_next(&feeder, &history, &value)) {
        assert(value.sequence == expected++);
        assert(value.status & PROTOCOL_STATUS_HISTORICAL);
        assert(!physical_feeder_next(&feeder, &history, &value));
        assert(physical_feeder_mark_sent(&feeder, expected - 1));
    }
    assert(expected == 301);
    for (uint32_t sequence = 301; sequence < 306; ++sequence) {
        protocol_physical_sample_t live = sample(sequence,
            UINT64_C(1000000) + (uint64_t)sequence * PHYSICAL_SAMPLE_PERIOD_US);
        assert(physical_history_push(&history, &live));
        assert(physical_feeder_next(&feeder, &history, &value));
        assert(value.sequence == sequence);
        assert(!(value.status & PROTOCOL_STATUS_HISTORICAL));
        assert(physical_feeder_mark_sent(&feeder, sequence));
    }
    assert(!physical_feeder_next(&feeder, &history, &value));
}

static void test_overwrite_reports_gap(void)
{
    physical_history_t history;
    physical_feeder_t feeder = {0};
    physical_history_clear(&history);
    for (uint32_t sequence = 0; sequence <= 300; ++sequence) {
        protocol_physical_sample_t value = sample(sequence,
            UINT64_C(1000000) + (uint64_t)sequence * PHYSICAL_SAMPLE_PERIOD_US);
        assert(physical_history_push(&history, &value));
    }
    assert(physical_feeder_begin(&feeder, &history, 1, 2,
        UINT64_C(2000000), UINT64_C(7000000)));
    for (uint32_t sequence = 301; sequence < 550; ++sequence) {
        protocol_physical_sample_t value = sample(sequence,
            UINT64_C(1000000) + (uint64_t)sequence * PHYSICAL_SAMPLE_PERIOD_US);
        assert(physical_history_push(&history, &value));
    }
    protocol_physical_sample_t value;
    assert(physical_feeder_next(&feeder, &history, &value));
    assert(value.sequence > 50);
    assert(value.status & PROTOCOL_STATUS_HISTORY_OVERWRITE);
    assert(feeder.lost_before_send != 0);
}

static void test_normal_rolling_eviction_is_not_reported_as_loss(void)
{
    physical_history_t history;
    physical_feeder_t feeder = {0};
    physical_history_clear(&history);
    for (uint32_t sequence = 0; sequence <= 500; ++sequence) {
        protocol_physical_sample_t value = sample(sequence,
            UINT64_C(1000000) + (uint64_t)sequence * PHYSICAL_SAMPLE_PERIOD_US);
        assert(physical_history_push(&history, &value));
    }
    assert(history.overwrite_count != 0);
    assert(physical_feeder_begin(&feeder, &history, 3, 4,
        UINT64_C(3000000), UINT64_C(8000000)));
    protocol_physical_sample_t value;
    assert(physical_feeder_next(&feeder, &history, &value));
    assert(feeder.lost_before_send == 0);
    assert(!(value.status & PROTOCOL_STATUS_HISTORY_OVERWRITE));
}

static void test_full_output_queue_retries_without_overtaking(void)
{
    physical_history_t history;
    physical_feeder_t feeder = {0};
    physical_history_clear(&history);
    for (uint32_t sequence = 0; sequence <= 300; ++sequence) {
        protocol_physical_sample_t value = sample(sequence,
            UINT64_C(1000000) + (uint64_t)sequence * PHYSICAL_SAMPLE_PERIOD_US);
        assert(physical_history_push(&history, &value));
    }
    assert(physical_feeder_begin(&feeder, &history, 5, 6,
        UINT64_C(2000000), UINT64_C(7000000)));

    protocol_physical_sample_t first, retried, second;
    assert(physical_feeder_next(&feeder, &history, &first));
    assert(first.sequence == 50);
    /* Model a nonblocking xQueueSend failure: release but do not advance. */
    assert(physical_feeder_retry(&feeder, first.sequence));
    assert(physical_feeder_next(&feeder, &history, &retried));
    assert(retried.sequence == first.sequence);
    assert(retried.timestamp_us == first.timestamp_us);
    assert(physical_feeder_mark_sent(&feeder, retried.sequence));
    assert(physical_feeder_next(&feeder, &history, &second));
    assert(second.sequence == first.sequence + 1U);
}

int main(void)
{
    test_history_to_live_exactly_once();
    test_overwrite_reports_gap();
    test_normal_rolling_eviction_is_not_reported_as_loss();
    test_full_output_queue_retries_without_overtaking();
    puts("sensor history tests: PASS");
    return 0;
}
