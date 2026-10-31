# Hardware core boundary

This component preserves recognizable C++ modules from `hardware/node` while
the original directory remains the canonical, untouched source. The copied
MPU6050 and generic-peak files retain the hardware interfaces and behavior;
`Mpu6050.cpp` and `GenericPeakDetector.cpp` are built directly as C++.

`hardware_bridge.cpp` is communications-owned. It is the only file here that
converts between hardware C++ objects and the C firmware. The bridge owns the
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
those files are not edited or forked. Every captured sample is offered to this
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
path uses phone -> Ultra96 -> both ESPs over the existing Wi-Fi/TLS connections,
so it does not require ESP-NOW LED mirroring.

## Wi-Fi-only communications runtime

Bluetooth SPP startup, polling, and the communications Bluetooth bridge have
been removed. The component no longer builds `hardware/node/NodePlatform.cpp`
or depends on the Bluetooth component. The copied MPU6050 driver uses FreeRTOS
delays directly, retaining its original initialization delays.

`sdkconfig.defaults` disables Bluetooth and software Wi-Fi/Bluetooth coexistence.
The `node1_integration_100HZ` and `node2_integration_100HZ` configurations have
also been regenerated with Bluetooth disabled. This releases the controller,
host, task, and queue memory for Wi-Fi/TLS. Existing generated configurations
for other profiles may still contain their older Bluetooth settings.

USB serial logging remains available through ESP-IDF. The removed Bluetooth
command interface is not replaced by a USB command parser in this change.
Future local feedback diagnostics must add their own command adapter. The
standalone hardware application's Bluetooth implementation is unchanged.

The checked-in `partitions_2mb.csv` provides a 0x1e0000-byte factory slot on
the boards' detected 2 MiB flash. This project already used a single factory
application, so the larger slot does not remove a previously available OTA
slot.

