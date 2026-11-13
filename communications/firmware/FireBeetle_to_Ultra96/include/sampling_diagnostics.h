#ifndef SAMPLING_DIAGNOSTICS_H
#define SAMPLING_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

/* Task-owned counters. Only completed copies are passed to the logger. */
typedef struct {
    uint64_t elapsed_us;
    uint64_t read_total_us;
    uint32_t report_id;
    uint32_t attempts, captured, stored, read_fail, history_reject;
    uint32_t skipped_slots, late_over_1ms;
    uint32_t late_max_us, wake_late_max_us, prework_max_us;
    uint32_t read_max_us, capture_max_us, processing_max_us;
    uint32_t loop_gap_max_us, capture_gap_max_us;
    uint32_t queue_high, enqueue_retries, history_lost_delta;
    uint32_t send_age_max_us;
} sampling_diagnostics_t;

static inline void sampling_diag_max(uint32_t *maximum, uint64_t value)
{
    uint32_t bounded = value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
    if (bounded > *maximum) *maximum = bounded;
}

static inline void sampling_diag_attempt(sampling_diagnostics_t *stats,
    uint64_t actual_us, uint64_t scheduled_us, uint32_t period_us)
{
    uint64_t late = actual_us > scheduled_us ? actual_us - scheduled_us : 0;
    stats->attempts++;
    sampling_diag_max(&stats->late_max_us, late);
    if (late >= 1000) stats->late_over_1ms++;
    /* Count full overdue slots, not every slightly late capture. This observes
     * the existing deadline/reset rule without changing its phase. */
    stats->skipped_slots += (uint32_t)(late / period_us);
}

static inline void sampling_diag_read(sampling_diagnostics_t *stats,
    uint32_t duration_us, bool ok)
{
    stats->read_total_us += duration_us;
    sampling_diag_max(&stats->read_max_us, duration_us);
    if (!ok) stats->read_fail++;
}

static inline double sampling_diag_hz(const sampling_diagnostics_t *stats)
{
    return stats->elapsed_us ?
        (double)stats->stored * 1000000.0 / (double)stats->elapsed_us : 0.0;
}

#endif
