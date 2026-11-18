# Stage 1 local feedback checkpoint

> **Historical checkpoint - Stage 1 local feedback.**
> This revision used the 100 Hz physical-integration baseline with local LED/
> buzzer diagnostics, before network feedback. Its rep/ready flashes and idle
> double-beep were later superseded. The original 31 October build-only report
> said physical checks were pending; the opening update records the user's
> subsequent confirmation on 1 November 2026. Original pending/stop-at-checkpoint
> statements below retain that chronology and do not describe current progress.
> All build/flash commands require the matching saved source/images; historical
> profiles are absent from the final configuration. See
> [current setup references](../../../../docs/README.md#component-guides) and
> [the consolidated checkpoint results](../../../../docs/EXPERIMENTS_AND_RESULTS.md#6-feedback-idle-and-fall-checkpoints).

Historical checkpoint: the user confirmed Stage 1 physical behavior on
2026-11-01. Its verified firmware and matching source are retained under
`.build/firmware_stages/stage1_verified_2026-11-01/`. Current Stage 2 source
removes the fallback guards described below; rebuild earlier behavior only
from its matching source snapshot. See `STAGE2_CHECKPOINT.md` for current code.

Stage 1 implements local outputs only. No Stage 2 network commands, real fall
detection, AR messages, or AI idle hook have been added. Boards have not been
flashed by the implementation workflow. Standalone `hardware/` is unchanged.

## Implementation

- `components/hardware_core/include/FeedbackController.h` and
  `src/FeedbackController.cpp`: allocation-free timing, canonical hardware
  durations, independent LED/buzzer channels, fall priority, and uint32 rollover.
- `FeedbackDiagnostic.h` / `FeedbackDiagnostic.cpp`: exact serial command
  parsing, 47-character maximum line, reject entire oversized/invalid line.
- `feedback_bridge.h` / `feedback_bridge.cpp`: C-compatible local trigger API,
  GPIO ownership, UART0 diagnostic adapter. Only Hip configures or writes GPIO26.
  GPIO2 is the LED on both boards. Outputs start inactive; no boot patterns.
- `src/physical_pipeline.c`: initializes feedback, services it on every loop
  before capture, regardless of valid samples or network feeder state. Commands
  are handled on the pipeline task, with at most 32 input bytes per loop, zero
  UART receive wait, 256-byte RX buffer, and no additional diagnostic task.
- `src/exercise_bridge.cpp`: disables exercise GPIO writes and local ready/rep
  triggers only when `APP_STAGE1_FEEDBACK=1`. Original profiles retain their
  original exercise GPIO writes, session feedback, and `led_on` status.
  In Stage 1, actual output status is available through `feedback status`.
- Component `CMakeLists.txt`: registers the controller, parser, bridge and UART
  dependency. `platformio.ini`: adds `node1_stage1` / `node2_stage1`, inheriting
  the respective 100 Hz integration flags and enabling serial diagnostics.
- `sdkconfig.node1_stage1` / `sdkconfig.node2_stage1`: copied from the respective
  integration_100HZ configurations; firmware `.gitignore` keeps these reviewable.
- `test_feedback.cpp`, `test_feedback_bridge.cpp`, `stubs/`, and
  `run_stage1_tests.ps1`: timing/parser checks and production bridge/GPIO tests,
  including actual hardware counter reps with no exercise GPIO writes.

Only `node1_stage1` and `node2_stage1` enable `APP_STAGE1_FEEDBACK=1` and USB serial
diagnostics. Original `node1_integration_100HZ` and `node2_integration_100HZ`
retain their pre-Stage 1 pipeline and exercise feedback behavior. Their config
files and PlatformIO environment definitions are unchanged. New Stage 1
feedback introduces no ESP-NOW mirroring.

The original profiles are the fallback builds. Their exercise bridge was
preprocessed with Stage 1 disabled and compared against the pre-Stage 1 Git
source; the resulting tokens match. Removing the disabled Stage 1 blocks from
the pipeline likewise reproduces the pre-Stage 1 pipeline. These are rebuilt
fallback binaries; the earlier build artifacts were overwritten during the
initial Stage 1 verification, so this does not recover their original bytes.
Both rebuilt original ELFs were also checked to contain no Stage 1 controller
or bridge symbols. Frozen copies of their bootloader, partition table, firmware,
configuration, SHA-256 manifest, and manual flash script are saved under the
repository's `.build/firmware_fallback/pre_stage1_2026-10-31/`, with one folder
per original environment. Ordinary PlatformIO builds do not overwrite these
copies; see that directory's `README.md` for flashing without rebuilding.

## Automated verification

Verified on 2026-10-31: all seven host test executables passed, all four firmware
profiles below built successfully, and `git diff --check` passed. Physical tests
remain pending. Final firmware build output contains no compiler warnings or
errors.

| Environment | Build | Static RAM (bytes) | Firmware flash (bytes) |
| --- | --- | --- | --- |
| `node1_stage1` | PASS | 76,544 | 866,339 |
| `node2_stage1` | PASS | 76,544 | 866,343 |
| `node1_integration_100HZ` | PASS | 76,472 | 851,979 |
| `node2_integration_100HZ` | PASS | 76,472 | 851,983 |

From the repository root, with desktop GCC/G++ installed:

```powershell
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage1_tests.ps1
```

Seven executables cover feedback, the GPIO/serial bridge, protocol, sequence-aware
history/feeder, 100 Hz generic peak detection, hardware sensor history, and the
exercise mock waveform. Feedback tests cover every millisecond boundary, all
patterns crossing uint32 wraparound, rep replacing ready, fall cancelling idle,
rejection of idle during fall, outputs expiring with no sensor calls, Node 2
never touching the buzzer GPIO, oversized line recovery, and real counted reps
producing no independent LED writes.

From `communications/firmware/FireBeetle_to_Ultra96`:

```powershell
pio run -e node1_stage1 -e node2_stage1
pio run -e node1_integration_100HZ -e node2_integration_100HZ
```

Firmware images are `.pio/build/<environment>/firmware.bin`. Host tests and
builds cannot establish physical pin wiring, buzzer sound, or actual acquisition
cadence. Output edges occur when the pipeline next services the controller;
task scheduling and sensor I/O may introduce small timing jitter. Diagnostics
are intended for manual bench commands, not sustained serial flooding.

## Historical physical test procedure

> **Status clarification:** The user subsequently confirmed Stage 1 physical
> behavior on 1 November 2026, as recorded above. This is the procedure for that
> saved revision; its output patterns do not describe final firmware. Optional
> instrumentation checks are not established merely by the general confirmation.

Use separate terminals for the boards. Substitute their actual COM ports below.
Run each upload only when ready to flash that board; these commands have not
been executed by the implementation workflow:

```powershell
pio run -e node1_stage1 -t upload --upload-port COM_HIP
pio device monitor -e node1_stage1 --port COM_HIP --baud 115200 --filter send_on_enter

pio run -e node2_stage1 -t upload --upload-port COM_WRIST
pio device monitor -e node2_stage1 --port COM_WRIST --baud 115200 --filter send_on_enter
```

Use `send_on_enter` (or another serial terminal configured to send whole lines)
and enter these exact commands. CR, LF, and CRLF line endings are accepted.

| Command | Both boards' LED | Hip buzzer | Wrist buzzer |
| --- | --- | --- | --- |
| `feedback status` | No new pattern; logs current output state | No new pattern | `supported=0` |
| `feedback rep` | On 200 ms, then off | Unchanged | Untouched |
| `feedback ready` | Three repetitions of off 100 ms / on 100 ms, then off | Unchanged | Untouched |
| `feedback idle` | Unchanged | On 100 ms / off 100 ms / on 100 ms, then off | Request rejected |
| `feedback fall-alarm` | Unchanged | On 3,000 ms, then off | Request rejected |

1. At boot, confirm no ready blinks or beep. With the sensor initialized, USB
   diagnostics should work before Wi-Fi/TLS connects and with streaming stopped.
2. Run every command while acquisition is running, then repeat after the
   existing Ultra96 STOP control. Confirm history/acquisition continue and
   diagnostics still complete; Stage 1 adds no serial STOP command.
3. Paste two lines `feedback idle` then `feedback fall-alarm` together on Hip.
   Confirm the final alarm lasts three seconds and no idle beep resumes. During
   the alarm, `feedback idle` must be rejected without shortening the alarm.
4. Paste `feedback ready` then `feedback rep` together. Confirm one 200 ms rep
   pulse replaces the ready pattern and no delayed ready blinks follow.
5. Trigger a rep and leave the board alone; it must turn off without another
   command. Once running, a sensor-read failure must not strand an output on.
   Observe pipeline error logs; do not disconnect powered sensor wiring merely
   to perform this check. A missing sensor at startup still prevents pipeline
   startup under the existing initialization policy.
6. Exercise normally without sending rep commands. Local counters must not add
   LED flashes. Confirm all buzzer commands leave Wrist's buzzer GPIO untouched.
7. Optionally observe GPIO2/GPIO26 with a logic analyzer to check durations and
   continued 100 Hz capture under normal manual diagnostic use.

`feedback fall-alarm` tests output only. It does not assert that a fall occurred
and creates no fall event. No teammate source is needed for this local stage.

Stop at this checkpoint. Stage 2 remains unimplemented pending the user's
physical results and instruction to proceed.
