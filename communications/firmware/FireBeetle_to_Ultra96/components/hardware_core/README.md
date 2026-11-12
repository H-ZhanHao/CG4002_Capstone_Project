# Hardware core boundary

This component builds shared C++ modules directly from `hardware/node`, which
remains the canonical, untouched source. `GenericPeakDetector.cpp` and the
counter, session, buzzer, fall and MPU6050 implementations are compiled from that
directory; their headers are included from there too. There are no copied
hardware headers or implementations in this component. The communications-owned
`src/node_platform_adapter.cpp` implements only `NodePlatform::delayMs` with
FreeRTOS, preserving the canonical MPU6050 driver's initialization delays without
building the standalone Bluetooth platform.

`hardware_bridge.cpp` is the communications-owned C ABI for sensor acquisition
and generic peak detection. The bridge owns the
MPU6050 and generic-peak objects, supplies deterministic mock samples, maps
`esp_timer` microseconds to the detector's millisecond API using the same
monotonic origin, and returns plain C sample and peak structures.

The communications-owned `src/sensor_history.c` is intentionally separate
from the hardware `SensorHistory` class. The original class prepares fixed
three-second snapshots without capture sequences. Transport history must retain
a sequence assigned at acquisition, continue accepting samples while a flush
drains, retry a rejected queue offer, and transition exactly once from history
to live. Keeping that adapter separate avoids changing the teammate's class or
bringing its overlapping-window workflow into the node runtime.

The firmware does not compile or call `SensorHistory.cpp`. The canonical
`SensorHistory.h` supplies `SensorSample` and the history constants used by
`GenericPeakDetector.h`; the history class itself is not instantiated.

The communications transport ring retains seven seconds at 100 Hz: three
seconds for the requested pre-peak window plus four seconds for peak
confirmation and command delivery. Its 701 entries live in static DRAM rather
than on the physical-pipeline task stack.

Do not add `hardware/main.cpp`, `StreamCoordination`, or `WindowScheduler` to
this component. The communications project retains one `app_main`, and
Ultra96 owns clock/flush coordination and overlapping windows.

## Exercise integration slice

`exercise_bridge.cpp` is the C ABI for the next integration phase. The
component builds the hardware teammate's original `RepCounter.cpp`,
`ContinuousCounter.cpp`, and `NodeSession.cpp` directly from `hardware/node`;
those files are not edited or forked. The feedback controller also builds the
canonical `hardware/node/BuzzerFeedback.cpp` directly. Every captured sample is offered to this
bridge by the existing physical pipeline, so there is still only one MPU6050
read and one acquisition clock.

Node 1 maps to the hip/waist role and Node 2 maps to the wrist role. Physical
builds start the counter session in standby. Mock builds complete a local
synthetic peer acknowledgement and use a deterministic movement that drives
both the pause and continuous counters, allowing counter and LED timing to be
tested without MPU6050s or provisioned peer MAC addresses.

The bridge exposes the hardware `LinkPacket` as an opaque, fixed 24-byte C
object and stores outgoing control events in a bounded 12-entry queue. These
legacy peer interfaces have no transport connected. The planned AR feedback
path uses phone -> Ultra96 -> Hip for rep beeps, and Ultra96 -> both ESPs for
steady activity LEDs, over the existing Wi-Fi/TLS connections. It does not
require ESP-NOW forwarding.

## Wi-Fi-only communications runtime

Bluetooth SPP startup, polling, and the communications Bluetooth bridge have
been removed. The component no longer builds `hardware/node/NodePlatform.cpp`
or depends on the Bluetooth component. The canonical MPU6050 driver uses the
communications delay adapter, retaining its original initialization delays.

`sdkconfig.defaults` disables Bluetooth and software Wi-Fi/Bluetooth coexistence.
The `node1_integration_100HZ` and `node2_integration_100HZ` configurations have
also been regenerated with Bluetooth disabled. This releases the controller,
host, task, and queue memory for Wi-Fi/TLS. Existing generated configurations
for other profiles may still contain their older Bluetooth settings.

USB serial logging remains available through ESP-IDF. Builds with
`APP_FEEDBACK_DIAGNOSTICS=1` also enable the bounded USB feedback command parser
described below. The standalone hardware application's Bluetooth implementation
is unchanged.

The checked-in `partitions_2mb.csv` provides a 0x1e0000-byte factory slot on
the boards' detected 2 MiB flash. This project already used a single factory
application, so the larger slot does not remove a previously available OTA
slot.

## Current integrated outputs

`FeedbackController` owns persistent activity LED state and delegates queued
100 ms rep beeps, 100 ms gaps and three-second fall priority to the canonical
`BuzzerFeedback`. Only Hip may drive the buzzer. Idle is silent; ready/rep LED
patterns and the idle double beep are removed. Session reset cancels rep sounds
and clears activity without silencing a fall. STOP clears activity locally too.
USB diagnostics use `feedback rep|active|idle|fall-alarm|status` and `fall inject`.
See the Ultra96 README for action IDs, deployment and physical checks.

## Fall detector integration

`fall_bridge.cpp` builds and calls the canonical `hardware/node/FallDetector.cpp`.
The physical pipeline passes raw acceleration (m/s^2), gyro (rad/s), and the
capture timestamp from the same IMU read. The adapter converts microseconds to
milliseconds; gyro bias correction stays inside the hardware detector.

Node 1 (Hip) reports a fall only after the hardware detector confirms freefall,
impact, a posture change of at least 45 degrees, and post-impact stillness.
Current hardware settings require a stable reference for 200 ms before a
candidate and confirmation at 500 ms after impact, with at least 200 ms settled.
The hardware `ExerciseConfig.h` is the source of these tuning values.

The bridge rejects missing/nonfinite sensor inputs, resets on non-increasing
timestamps or gaps exceeding the hardware's 100 ms limit, and suppresses repeat
events during the three-second alarm. The existing local buzzer and device-event
delivery path receives only confirmed falls. Node 2 (Wrist) does not report falls.

Run the portable bridge and canonical detector regression tests from the repo
root with:

```powershell
& ./communications/firmware/FireBeetle_to_Ultra96/fall_tests/run_tests.ps1
```

The `node1_update_fall` (Hip) and `node2_update_fall` (Wrist) PlatformIO
environments extend the existing `node1_one_node` and `node2_one_node` settings:
physical IMU, 100 Hz capture, Wi-Fi/TLS, independent startup/live joining, and
feedback diagnostics. Each has its own SDK configuration seeded from the
corresponding one-node configuration. Existing environment definitions and
the default environment are unchanged.

From `communications/firmware/FireBeetle_to_Ultra96`, build both profiles with:

```powershell
pio run -e node1_update_fall -e node2_update_fall
```

Upload each profile to its matching board (replace the COM port):

```powershell
pio run -e node1_update_fall -t upload --upload-port COM_HIP
pio run -e node2_update_fall -t upload --upload-port COM_WRIST
```

The updated bridge is shared source, so rebuilding an existing environment
also compiles the updated detector. `fall inject` checks alarm/event delivery;
it bypasses sensor-based detection and does not validate the new confirmation.

## Cleanup profiles

`node1_cleanup` and `node2_cleanup` extend their corresponding `update_fall`
environments: physical IMU, 100 Hz capture, a 1 ms scheduler tick, Wi-Fi/TLS,
independent startup/live joining, feedback diagnostics and the canonical fall
detector. Their SDK configurations are seeded from the matching `update_fall`
configurations. Build both from the firmware project directory:

```powershell
pio run --disable-auto-clean -e node1_cleanup -e node2_cleanup
```

Each profile has its own `sdkconfig.node*_cleanup` and
`.pio/build/node*_cleanup` outputs. Existing environment definitions, SDK
configurations and the default environment are preserved.
The source cleanup is shared: rebuilding an older profile also uses the
canonical hardware files and delay adapter. Communications history continues
to use `src/sensor_history.c`.

Use `--disable-auto-clean` when retaining built images across profile additions:
PlatformIO otherwise clears its shared build directory when the project
configuration checksum changes.
