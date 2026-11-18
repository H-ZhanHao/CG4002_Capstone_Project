# Hardware core boundary

Current component reference, reviewed against the repository on 18 November 2026.
This component compiles selected modules directly from `hardware/node`, using
the source list in [CMakeLists.txt](CMakeLists.txt). Communications-owned bridges
connect those modules to the existing acquisition, transport and feedback pipeline.

The tables below describe current source ownership. Integration decisions and
historical verification are documented in the
[hardware-communications vertical-slice record](../../../../docs/handoffs/HARDWARE_COMMUNICATIONS_VERTICAL_SLICE.md).
Build and flashing instructions are in the [firmware guide](../../FIRMWARE_GUIDE.md).

## Source and ownership

| Area | Current boundary |
| --- | --- |
| Canonical hardware | BuzzerFeedback, ContinuousCounter, NodeSession, RepCounter, FallDetector, GenericPeakDetector and Mpu6050 compile directly from `hardware/node`; headers come from there too. |
| `src/hardware_bridge.cpp` | C ABI for acquisition and generic peaks; owns the sensor/detector objects and one monotonic capture origin. |
| `src/node_platform_adapter.cpp` | Implements the canonical driver's delay function through FreeRTOS, without compiling the standalone Bluetooth platform. |
| `src/exercise_bridge.cpp` | Reuses canonical counter/session processing on the already-captured sample; physical counter sessions start in standby and peer interfaces have no live radio transport. Phone/AR supplies the authoritative rep events. Local counters do not independently drive final LEDs/buzzer outputs. |
| `src/feedback_bridge.cpp` and FeedbackController | Pipeline-owned GPIO/output state, connection LEDs and USB diagnostics. Canonical BuzzerFeedback owns queued rep sounds and fall priority. |
| `src/fall_bridge.cpp` | Hip-only detector adapter using the same acquisition; validity/gap/runtime reset and event/alarm handoff. |
| Firmware `src/physical_pipeline.c` | Sole task applying acquisition/control/feedback state and feeding history. |
| Firmware `src/tcp_client.c` | Sole connection owner; callbacks hand bounded work to the pipeline. |

The standalone hardware application, Bluetooth SPP and ESP-NOW transports,
hardware stream coordinator/window scheduler and C++ SensorHistory implementation
are excluded. See [integration decisions](../../../../docs/handoffs/HARDWARE_COMMUNICATIONS_VERTICAL_SLICE.md#integration-decisions)
for the reasons and replacement paths.

The transport ring is firmware `src/sensor_history.c`, not the canonical C++
SensorHistory class. It retains capture sequences, retries nonblocking queue
handoff and transitions from retained history to live data in order. Its 701
static entries retain seven seconds at 100 Hz; the normal pre-peak request is
three seconds. The hardware SensorHistory header
supplies shared sample/constants, but its implementation is not instantiated by
the firmware. Canonical SensorHistory host tests are separate regression checks.

## Final build configuration

Use `node1_final` and `node2_final` from firmware `platformio.ini`. Both enable
100 Hz physical acquisition, a 1 ms scheduler tick, TLS, independent joining,
idle resume and serial diagnostics. The final profiles enable runtime fall
control; Hip starts detection ON. Four additional sampling diagnostic profiles
retain the comparison configurations described in [Sampling diagnostics](../../SAMPLING_DIAGNOSTICS.md).
Historical stage/one-node/cleanup/update profiles are no longer environments.

Bluetooth is disabled in the retained SDK defaults/configurations. The
communications component has no Bluetooth dependency. The checked-in 2 MiB
partition map provides one `0x1e0000`-byte factory slot and coredump space.

## Current integrated outputs

Only the pipeline updates feedback/GPIO state. Connection retry is a slow blink;
HELLO acceptance produces three fast pulses. After that, each LED indicates
its acknowledged activity state. Idle is silent. One accepted phone rep increase
gives Hip a 100 ms beep; rapid beeps are queued with 100 ms gaps. Wrist does not
drive the buzzer. Fall alarm priority discards queued rep sounds and suppresses
new rep sounds during the three-second alarm. Session/STOP cleanup must not
silence a currently active local fall alarm.

USB diagnostics are `feedback rep`, `feedback active`, `feedback idle`,
`feedback status`, `feedback fall-alarm` and `fall inject`. The last two test
sound-only versus marked synthetic event delivery respectively; injection is
not physical fall detection and is filtered from phone alerts. Retired ready/
rep LED actions and idle-beep actions must not be restored from old checkpoints.
See [server feedback behavior](../../../../ultra96/README.md#integrated-activity-and-rep-outputs).

## Fall detector integration

The adapter calls canonical FallDetector with acceleration magnitude and a
millisecond timestamp derived from the capture clock. The restored thresholds
are below 4 m/s² for at least 100 ms, then impact above 25 m/s². Impact is tested
before nominal one-second wait expiry on that update, preserving the canonical
algorithm. There is no posture confirmation, learned gravity or gyro gate.

The adapter separately checks finite acceleration/gyro, increasing timestamps,
a 100 ms maximum sample gap, runtime reset and a three-second alarm cooldown.
Only Hip emits events or drives its buzzer. Server ON/OFF applies through the
pipeline and a matching ACK; unknown setting after reconnect is not an OFF
inference.

## Verification

Run [portable firmware checks](../../test/protocol_host/README.md), including
`fall_tests/run_tests.ps1` from the repository root. Source/build/host checks do
not establish physical GPIO timing, worn-sensor detection quality, sustained
100 Hz cadence or live phone acceptance. Retain those measurements separately.
