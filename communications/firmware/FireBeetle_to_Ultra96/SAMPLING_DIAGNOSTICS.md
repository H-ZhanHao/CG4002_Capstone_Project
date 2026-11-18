# Sampling diagnostics

Reviewed against the repository on 16 November 2026. Use the
[network setup guide](../../docs/NETWORK_SETUP.md) for the private route and
[final firmware guide](FIRMWARE_GUIDE.md) for normal builds. This document
defines measurement procedures; observed results belong in
[Experiments and results](../../docs/EXPERIMENTS_AND_RESULTS.md).

The `node1_sampling_diag` and `node2_sampling_diag` environments
preserve the original sampling baseline and add `APP_SAMPLING_DIAGNOSTICS=1`.
They share the direct Wi-Fi/TLS pipeline flags with the final firmware, but
retain the earlier coordinated-join behavior and fall detection enabled at boot.
They do not enable the final profiles' `APP_ONE_NODE` or
`APP_FALL_RUNTIME_CONTROL` options. Sensor capture remains at 100 Hz.
All measurement fields and the logger task compile out in the final builds.
Historical environment definitions have been removed from `platformio.ini`;
the `sdkconfig.node1_base` and `sdkconfig.node2_base` files remain as inputs
to the retained builds.

To restore normal firmware, upload `node1_final` / `node2_final` to the same
respective ports. USB flashing and physical testing are separate from building.
No Ultra96/server deployment is needed for the diagnostics.

The final server still uses independent readiness, but these diagnostic builds
do not accept the final firmware's live-only join command. For comparable runs,
bring both diagnostic boards up and synchronized before triggering capture.
Do not use these builds as proof of final dynamic joining or runtime fall control.

## Experimental 1,000 Hz scheduler tick

`node1_sampling_tick1000_diag` and `node2_sampling_tick1000_diag` inherit the
diagnostic profiles above. The FreeRTOS scheduler tick changes from 100 Hz
(10 ms) to 1,000 Hz (1 ms); sensor capture remains at **100 Hz / 10,000 us**.
The diagnostics, sampling-loop code, sensor reads, and task priorities are the
same. With the existing `vTaskDelay(1)`, the loop can run more often between
capture deadlines and offer historical samples to the transport more quickly.
This experiment therefore measures the effect of the faster tick on the
existing complete pipeline, including history catch-up.

Each profile generates its own `sdkconfig.nodeN_sampling_tick1000_diag` and build
directory. Its SDK defaults are layered from `sdkconfig.defaults`, the matching
base SDK config, then `sdkconfig.tick1000.defaults`. The final
defaults file changes only `CONFIG_FREERTOS_HZ`. Existing base and original
diagnostic SDK configs are not edited. Do **not** seed these new SDK configs
using the earlier copy commands: an existing generated config can take
precedence over defaults and retain the old tick rate.

From `communications/firmware/FireBeetle_to_Ultra96`:

```powershell
pio run -e node1_sampling_tick1000_diag -e node2_sampling_tick1000_diag
pio run -e node1_sampling_tick1000_diag -t upload --upload-port COM_NODE1
pio run -e node2_sampling_tick1000_diag -t upload --upload-port COM_NODE2
```

Use the same 115200-baud monitor commands above. Confirm **both** boards print:

```text
Sampling diagnostics enabled: tick_hz=1000 period_us=10000 source=MPU6050
```

Capture 30-60 seconds of streaming/movement and a separate idle period under
conditions comparable to the 100 Hz tick run. Keep the server's sample rate,
window length, and acceptance settings the same. Compare elapsed-time-weighted
capture rate (`sum(stored) * 1000000 / sum(elapsed_us)`), `skipped_slots`,
capture gaps, queue retries, and actual window rejection frequency. Assess
rates over several reports; the first report and interval boundaries can
contain an extra sample. Detailed diagnostics add the same instrumentation
to both runs, but this does not guarantee identical logging overhead.

Restore the earlier diagnostic experiment with `node1_sampling_diag` /
`node2_sampling_diag` (100 Hz tick), or normal firmware with
`node1_final` / `node2_final`. Flashing and physical results still require
the boards; a successful build alone does not establish an improvement.

## Collect a run

Check the startup line says `Sampling diagnostics enabled` and identifies the
node's tick rate, sample period, and sensor source. Retain serial logs from
both boards plus the server log. Collect an idle baseline, then normal movement
with streaming for at least 30-60 seconds, including rejected windows.
Avoid typing feedback diagnostic commands during the baseline; their logging
and processing may affect timing. Compare idle/streaming periods separately.

A lower-priority task prints one `SAMPLE_DIAG node=...` record per measurement
interval (approximately one second). The sampling task only updates counters,
reads timestamps, and copies a snapshot into a one-entry queue without waiting.
If logging stalls, the newest snapshot replaces an older pending snapshot;
gaps in `report` identify omitted intervals. Even these diagnostics add some
overhead and share the normal serial logger, so confirm improvement with a
normal build after the cause is fixed.

## Fields

Counters and maxima reset after each snapshot, except `report`, which increases
throughout the boot. Successful-capture and loop intervals retain the previous
timestamp across report boundaries so boundary gaps remain visible.

| Field | Meaning |
| --- | --- |
| `elapsed_us`, `hz` | Actual interval duration; successfully stored samples / elapsed time. A single interval may differ by one sample at a boundary; assess several reports. |
| `attempts`, `captured`, `stored` | Capture calls, successful captures, and samples successfully added to history. |
| `read_fail`, `history_reject` | Failed sensor reads and captures rejected by history. Mock builds time mock generation instead of I2C. |
| `skipped_slots` | Sum of full 10 ms slots overdue at the existing scheduler's capture decision. Does not count failed reads or every slightly late capture; not the same as a window's total deficit. |
| `late_over_1ms`, `late_max_us` | Attempts at least 1 ms late and maximum lateness at the scheduler decision. |
| `wake_late_max_us` | Maximum lateness relative to the next capture deadline at loop entry, before feedback/command processing. Includes all reasons the task did not reach loop entry sooner; does not alone prove scheduler starvation. |
| `prework_max_us` | Maximum time from loop entry to a due capture decision, including feedback/command work. |
| `read_mean_us`, `read_max_us` | Time immediately around the MPU6050 read, including failed attempts; mean uses all attempts. |
| `capture_max_us` | Full bridge capture duration, including sensor read and peak detection. |
| `processing_max_us` | Work after the bridge returns, including fall/exercise processing, history insertion, and trigger handling. |
| `loop_gap_max_us`, `capture_gap_max_us` | Longest loop-entry interval and successful-capture timestamp interval. A healthy capture interval is approximately 10,000 us. |
| `queue_high`, `enqueue_retries` | Highest observed sample queue occupancy and failed nonblocking enqueue attempts. Retries are not automatically dropped samples. Occupancy is sampled and may miss short peaks. |
| `send_age_max_us` | Maximum sample age when offered to the transport queue. Includes intentional pre-trigger history; this is not TCP acknowledgement or network latency. Watch for growth after the initial flush. |
| `history_lost_delta` | Unsent samples that expired from active feeder history during this report, accounting for feeder resets. Ordinary rolling-history eviction is excluded. |

Fast reads with late/skipped captures suggest scheduling or other task work.
Large read times/failures suggest I2C problems. Stable capture rate with growing
sample age, queue retries, or history loss suggests delivery backpressure.
This summary cannot attribute CPU time to individual Wi-Fi/TLS tasks.
