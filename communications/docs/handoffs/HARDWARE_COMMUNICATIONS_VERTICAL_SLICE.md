# Hardware–communications vertical slice

This record explains why the communications firmware reuses selected
`hardware/` modules while retaining its existing runtime and transport paths.
Reviewed against the repository on 18 November 2026.

The [hardware_core README](../../firmware/FireBeetle_to_Ultra96/components/hardware_core/README.md#source-and-ownership)
is the current reference for compiled modules, adapter responsibilities, build
configuration and output behavior. For build/flashing commands use the
[firmware guide](../../firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md).
The [historical record below](#historical-implementation-and-verification-record)
preserves earlier implementation and verification evidence; this documentation
review does not establish new physical acceptance.

## Integration decisions

| Decision | Reason and replacement path |
| --- | --- |
| Reuse hardware modules within the communications application rather than importing `hardware/main.cpp` and the full ExerciseNode loop | The existing communications entry point, acquisition task, sessions and bounded command queues remain the runtime owners. This avoids a second application entry point, duplicate acquisition and competing sensor/GPIO control. |
| Use existing Wi-Fi/TLS and USB diagnostics instead of Bluetooth SPP | Sensor/control traffic already passes through the laptop TLS relay and SSH route to Ultra96; phone interaction uses the WSS gateway. USB serial supplies local diagnostics. Excluding WirelessStreamer and the full Bluetooth platform avoids another telemetry/command path and preserves controller/host/queue RAM for Wi-Fi/TLS. A small platform delay adapter supplies the sensor driver's required initialization delays. |
| Give each wearable an independent connection instead of ESP-NOW peer activation/mode/feedback transport | Ultra96 already sends capture/activity commands and routes phone rep feedback over each node's existing TLS connection. NodeRadio and its peer handshake are unnecessary for this path; a usable node does not need its partner's activation acknowledgement. Compiling NodeSession does not connect the excluded radio transport. |
| Keep clock and capture coordination on Ultra96 | The existing coordinator synchronizes each node and owns ARM, START_FLUSH and STOP on a shared timeline. Importing the hardware StreamCoordinator/NodeStreamControl would introduce a second coordinator and potentially conflicting state. |
| Construct overlapping AI windows on Ultra96 instead of using the hardware WindowScheduler | Ultra96 reuses received timestamped samples for overlap. The ESP32 can drain history into live transmission in acquisition order without repeatedly sending overlapping window snapshots. |
| Retain communications `src/sensor_history.c` instead of the hardware C++ history implementation | Transport needs capture sequences, retryable acceptance into a bounded queue and a continuous oldest-first history-to-live cursor. The hardware fixed-window snapshot interface does not supply that drain contract. Shared hardware sample/configuration types can still be included without instantiating its history class. |
| Keep phone/AR as the rep-count authority and the communications pipeline as the output owner | Retained hardware counter/session modules are not an independent source of final rep feedback. Rep events return through WSS, Ultra96 and the wearable TLS connection; pipeline-owned feedback handles the physical outputs. A second local-counter or peer output trigger could duplicate sounds/flashes or overwrite activity/fall state. The canonical BuzzerFeedback implementation remains reusable within that ownership. |

These decisions apply to the communications firmware; the standalone hardware
application remains a separate build. Current source/configuration details are
maintained in the component README rather than repeated here. The broader
development sequence is in [Design evolution](../DESIGN_EVOLUTION.md#5-physical-hardware-integration-and-timing).

## Historical implementation and verification record

The following sections retain the initial 50 Hz/five-second-flush checkpoint
and intermediate exercise/Bluetooth builds. Descriptions of enabled Bluetooth,
old profiles, GPIO ownership, memory figures and pending checks apply to those
recorded revisions, not to the current integration described above. Reproducing
them requires their matching historical source/configuration; these retained
observations do not establish final physical acceptance.

### Initial additive wire contract

All integers and IEEE-754 float32 values are network byte order. Capture time,
Session ID and Sequence remain in the common 24-byte header.

| Message | ID | Payload |
|---|---:|---|
| `ACTIVITY_TRIGGER` | `0x05` | `!IIQ`: boot ID, arm generation, local peak time |
| `START_FLUSH` | `0x06` | `!QqIB3xI`: Ultra96 peak time, node-minus-Ultra96 offset, history duration, trigger node, reserved, trigger sequence |
| `PHYSICAL_DATA` | `0x12` | `!ffffffH`: acceleration m/s², angular rate rad/s, status |

The C and Python codecs reject incorrect lengths and non-finite physical
values. Ultra96 also rejects implausible ranges, stale Sessions, replayed
sequences and timestamp regressions. Trigger identity includes node, Session,
header Sequence, boot ID and arm generation; an exact duplicate is idempotent,
while reused identity with changed contents is an error.

Physical status bits currently include sensor valid (`0x0001`), historical
flush (`0x0002`) and history overwrite/gap (`0x0004`). Existing high status
bits continue to identify mock/benchmark data.

### Initial acquisition and feeder behavior

The current rate is 50 Hz (20,000 µs), not 100 Hz. The communications-owned
`sensor_history.c` owns a
401-entry ring that retains
slightly more than eight seconds. Capture Sequence is assigned once before a
sample enters that ring. On START_FLUSH the feeder selects the earliest stored
sample at or after `peak - duration`, exposes samples oldest-first and advances
only after the bounded transport queue accepts the offered item. Acquisition
continues while it drains. The same cursor then follows the live edge, which
prevents duplication and prevents live data overtaking history.

The acquisition task never waits for network I/O. Its command queue has eight
entries, its trigger queue has two, and the outbound queue has 50. Routine
age-based eviction from the rolling history is expected and is not reported as
loss. If an active feeder falls behind far enough that unsent capture sequences
are evicted, those skipped sequences are counted and reported in
status/heartbeat telemetry. A quiet physical node before `START_FLUSH` is also
intentional and its heartbeat does not set `SOURCE_UNAVAILABLE`. STOP disables
the feeder and increments the local arm generation; it does not stop bounded
acquisition. Disconnect ends the Session, and reconnect requires a new HELLO
and clock mapping.

The 401-entry ring is task-owned but stored in static DRAM, not as a local
variable on the acquisition task's 4 KiB stack. An earlier vertical-slice
build placed it on that stack and reset immediately with an interrupt-watchdog
panic when the first queue operation encountered corrupted task memory.

### Initial mock mode

`node1_mock_tls` and `node2_mock_tls` compile the real acquisition/history,
peak detector, feeder, codecs, TLS client and command path. Their deterministic
50 Hz source periodically injects a synthetic motion excursion that the
complete hardware generic-peak detector converts into ACTIVITY_TRIGGER. No mock-only coordinator
or alternate transport bypasses the vertical slice.

### Initial verification completed without MPU6050s

- Production C codec tests, including independent golden vectors for all three
  messages, malformed payloads and NaN/Inf rejection.
- Host C history/feeder tests for five seconds of history followed by live
  samples, chronological exactly-once sequences and overwrite/gap reporting.
- Existing hardware host tests for exercise presets/profiles, node session and
  startup, sensor history, generic peak, MPU6050 conversion, window scheduling,
  stream coordination and wireless streaming.
- Ultra96 protocol/window tests, including matching Python golden vectors,
  two simulated nodes, stale Session, replay/duplicate handling, timestamp
  regression, trigger idempotency, STOP/re-arm, mapped five-second history and
  live samples.
- Laptop test discovery.
- PlatformIO builds for both private TLS physical profiles. The mock profiles
  use the same build graph and are also part of the verification matrix.

The complete Ultra96 discovery additionally imports the visualizer gateway;
that test requires the pinned `websockets` dependency from
`ultra96/requirements.txt`. Its absence in a developer Python environment is a
dependency/setup failure, not a sensor-server test failure.

### Physical verification deferred at the initial checkpoint

When two MPU6050s are available, verify WHO_AM_I and wiring on each board;
physical unit scale, sign and body-axis orientation; sustained 50 Hz cadence
and queue headroom; peak threshold/refractory behavior for each exercise;
five-second history coverage at startup; cross-node alignment and periodic
clock refresh under relay load; STOP/disconnect/reconnect on hardware; and a
long run that proves no unexplained duplicates, gaps or overtaking.

The original hardware Bluetooth and ESP-NOW activation/mode/LED modules have
not been moved or deleted. Bluetooth is now connected through the Phase 2B
adapter below. ESP-NOW remains a separate physical integration step and is
reserved for the teammate's activation/mode/LED peer control; it is not used
for IMU-stream coordination.

### Phase 2A: exercise-core integration

The first part of the next phase now runs the teammate's original
`RepCounter`, `ContinuousCounter`, exercise presets, and `NodeSession` inside
the communications firmware. They are built directly from `hardware/node`
without editing those source files. Communications-owned
`components/hardware_core/src/exercise_bridge.cpp` provides the C boundary and
is called once for every sample captured by `physical_pipeline.c`; it does not
read the MPU6050 independently and it does not alter TLS/history/flush state.

The concrete role mapping is Node 1 = hip/waist and Node 2 = wrist. The bridge
owns GPIO 2 LED timing from `ExerciseConfig.h`, a bounded 12-packet peer-control
queue, counter activation/mode/exercise state, and counter status. The original
24-byte `NodeSession::LinkPacket` is exposed opaquely to C so a radio adapter
cannot depend on C++ enum layout. Physical builds remain in standby until an
activation command and matching peer acknowledgement arrive.

Mock builds deliberately synthesize only that peer acknowledgement. They then
feed the exact counter algorithms a deterministic eight-second waveform that
contains one valid pause-mode movement and one valid continuous-mode cycle.
The mock does not synthesize Ultra96 `START_FLUSH`; the generic detector still
emits `ACTIVITY_TRIGGER`, and Ultra96 remains the only component that sends
`START_FLUSH`.

Phase 2A verification completed without IMUs:

- Original exercise preset and profile host tests pass.
- Original `NodeSession` loss/reorder/duplicate/LED/mode/activation test passes.
- New mock-waveform host test proves exactly one pause rep and one continuous
  rep using the original counters.
- `node1_mock_tls` and `node2_mock_tls` build at 11.7% RAM and 86.3% of the
  current 1 MiB application partition.
- `node1_private_tls` and `node2_private_tls` build at 11.8% RAM and 87.7% of
  that partition, with the mock activation path compiled out.

### Phase 2B: intermediate Bluetooth integration

Phase 2B enables Classic Bluetooth SPP while retaining the detected 2 MiB flash
limit. `partitions_2mb.csv` keeps NVS and PHY storage, assigns 0x1e0000 bytes to
the single factory application, and reserves the final 64 KiB for coredumps.
The hardware teammate's original `NodePlatform.cpp` is compiled directly;
`bluetooth_bridge.cpp` applies its bounded queued input only from the physical
pipeline task. Commands preserve local exercise, mode, activation, standby,
reset, status, and LED/counter behavior, while Ultra96 remains the only
clock/flush coordinator.

Phase 2B verification completed without MPU6050s:

- Both mock TLS profiles build at 18.4% RAM and 73.7% of the 0x1e0000-byte
  application partition.
- Both private TLS profiles build at 18.4% RAM and 74.3% of that partition.
- The generated image command explicitly uses a 2 MiB flash size; the
  PlatformIO board-description banner still reports its generic manifest
  value and is not the image or partition setting.
- Protocol/history/exercise host tests, the original hardware host tests, and
  all 51 Ultra96 server tests pass. Actual SPP pairing and command round trips
  still require testing on the ESP32 boards, but do not require MPU6050s.

The live ESP-NOW adapter remains deferred and requires confirming which
provisioned pair is current: the communications secrets name
`...B6:8C`/`...DD:80`, while the hardware configuration names
`...55:BC`/`...4E:B8`. No MAC pair is silently selected.
