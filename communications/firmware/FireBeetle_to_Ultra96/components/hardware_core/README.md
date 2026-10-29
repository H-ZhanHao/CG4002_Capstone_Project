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
five-second snapshots without capture sequences. Transport history must retain
a sequence assigned at acquisition, continue accepting samples while a flush
drains, retry a rejected queue offer, and transition exactly once from history
to live. Keeping that adapter separate avoids changing the teammate's class or
bringing its overlapping-window workflow into the node runtime.

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
object and stores outgoing control events in a bounded 12-entry queue. A later
ESP-NOW adapter must copy callback input into a bounded queue and deliver it to
the physical-pipeline task; callbacks must never call bridge state methods.
Bluetooth commands must follow the same command-queue ownership rule.

## Bluetooth SPP integration

The communications target builds the hardware teammate's original
`hardware/node/NodePlatform.cpp` directly. Its `BluetoothSpp` implementation
retains pairing, connection, congestion, and bounded byte/line queues without
copying or editing that source. The communications-owned
`bluetooth_bridge.cpp` supplies only the command adapter needed by this runtime.

SPP callbacks enqueue bytes in the original bounded queue. The physical
pipeline task calls `bluetooth_bridge_poll`, parses at most 64 bytes per pass,
and is the only context that changes exercise/counter/LED state. Bluetooth can
select exercises and modes, activate/standby/reset the local exercise session,
and report status. It cannot start an IMU flush or replace Ultra96 as stream and
clock coordinator.

The checked-in `partitions_2mb.csv` provides a 0x1e0000-byte factory slot on
the boards' detected 2 MiB flash. This project already used a single factory
application, so the larger slot does not remove a previously available OTA
slot.

