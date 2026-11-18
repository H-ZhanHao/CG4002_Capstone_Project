#include "sampling_diagnostics.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    sampling_diagnostics_t stats = {};
    // Sub-period jitter must not be reported as a skipped sampling slot.
    sampling_diag_attempt(&stats, 10200, 10000, 10000);
    sampling_diag_attempt(&stats, 29999, 20000, 10000);
    assert(stats.skipped_slots == 0 && stats.late_over_1ms == 1);
    // The same floor rule as the existing scheduler's overdue-slot reset.
    sampling_diag_attempt(&stats, 50000, 30000, 10000);
    assert(stats.attempts == 3 && stats.skipped_slots == 2);
    assert(stats.late_max_us == 20000 && stats.late_over_1ms == 2);

    sampling_diag_read(&stats, 1600, true);
    sampling_diag_read(&stats, 50000, false);
    sampling_diag_read(&stats, 1800, true);
    assert(stats.read_fail == 1 && stats.read_max_us == 50000);
    assert(stats.read_total_us == 53400);

    assert(sampling_diag_hz(&stats) == 0); // No division by zero at startup.
    stats.stored = 100;
    stats.elapsed_us = 1050000;
    assert(std::abs(sampling_diag_hz(&stats) - 95.238095238) < 0.000001);
    // An extended stall must saturate rather than wrap a duration counter.
    sampling_diag_max(&stats.capture_gap_max_us, (uint64_t)UINT32_MAX + 100);
    assert(stats.capture_gap_max_us == UINT32_MAX);

    // Starting the next report does not carry interval counters forward.
    stats = {};
    sampling_diag_attempt(&stats, 60000, 60000, 10000);
    assert(stats.attempts == 1 && stats.skipped_slots == 0);
    assert(stats.read_fail == 0 && stats.late_max_us == 0);
    std::puts("Sampling diagnostic checks passed");
}
