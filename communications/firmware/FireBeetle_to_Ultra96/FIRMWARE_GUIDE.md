# Hip and Wrist firmware guide

Reviewed against the repository on 16 November 2026. These instructions build
the final independent-node firmware in this directory, not the standalone
root hardware project. Follow [Network setup](../../docs/NETWORK_SETUP.md)
for the private router, IPs, firewall, TLS identities and laptop/SSH route.

## Identity, hardware and profiles

| Board | Protocol ID | Source mask | Final environment |
| --- | ---: | --- | --- |
| Hip/Thigh | 1 | `0x02` | `node1_final` |
| Wrist | 2 | `0x04` | `node2_final` |

Each ESP32 connects directly to the laptop TLS relay. No peer/leaf or ESP-NOW
IMU-forwarding transport is connected in this firmware. Each board owns one
MPU6050 on I2C: SDA IO21, SCL IO22, VCC 3.3 V, GND common, AD0 low for address
`0x68`. Confirm the actual board/sensor wiring and body-axis orientation.
GPIO2 is the LED; only Hip drives the GPIO26 buzzer output. Check the physical
buzzer circuit and polarity against the hardware design before use.

[platformio.ini](platformio.ini) selects ESP-IDF, a FireBeetle32 target,
2 MiB flash and [partitions_2mb.csv](partitions_2mb.csv). Its single factory
application slot is `0x1e0000` bytes. Final environments enable physical IMU,
TLS, independent joining, idle resume, USB feedback diagnostics and runtime
fall control. Sensor acquisition is 100 Hz; scheduler tick is 1000 Hz (1 ms).

Only these environments are retained:

- `node1_final`, `node2_final` for normal operation.
- `node1_sampling_diag`, `node2_sampling_diag` for the earlier 100 Hz scheduler-tick baseline.
- `node1_sampling_tick1000_diag`, `node2_sampling_tick1000_diag` for the faster-tick diagnostic comparison.

Old stage, one-node, cleanup, mock and campus-association names in historical
records are not current build environments. Diagnostics differ from final
runtime flags; see [Sampling diagnostics](SAMPLING_DIAGNOSTICS.md).

## Configure private headers

Under `include/`, supply ignored `wifi_secrets.h`,
`laptop_relay_endpoint_secrets.h` and `demo_tls_config.h` as described in
[network configuration](../../docs/NETWORK_SETUP.md#2-firmware-network-configuration).
The firmware expects the laptop at `192.168.0.2:15000`, verifies its configured
TLS identity and requires real public trust material. The example certificate
is not usable. Credentials/keys must stay outside committed documentation.
The application's shared development token is a predictable demo identifier,
not a unique secret per board.

## Build, flash and monitor

Run from the **repository root**, with PlatformIO available:

```powershell
pio run -d communications/firmware/FireBeetle_to_Ultra96 -e node1_final -e node2_final
```

For each board, replace `COM_HIP` or `COM_WRIST` with its actual port:

```powershell
pio run -d communications/firmware/FireBeetle_to_Ultra96 -e node1_final -t upload --upload-port COM_HIP
pio run -d communications/firmware/FireBeetle_to_Ultra96 -e node2_final -t upload --upload-port COM_WRIST
pio device monitor -d communications/firmware/FireBeetle_to_Ultra96 --port COM_HIP --baud 115200 --filter send_on_enter
```

Use a separate terminal for Wrist monitoring. Close its monitor before uploading
on that port. Outputs are `.pio/build/node1_final/` and `.pio/build/node2_final/`
inside this firmware project. Save verified binaries and matching source/config
outside `.pio/build` before later builds; build environments do not freeze shared
source. Building does not flash hardware or establish physical acceptance.

## Runtime sequence and recovery

Start [one Ultra96 server](../../ultra96/README.md#choose-one-sensor-server),
the gateway if using a phone, the SSH tunnel and the laptop TLS relay. Each
wearable captures continuously into a bounded seven-second (701-entry) history.

1. Wi-Fi/TLS/HELLO establish an independent node session.
2. Ultra96 estimates that node's clock mapping and sends ARM without waiting
   for a partner. Five synchronization exchanges target a robust offset from
   the three lowest-delay usable rounds.
3. A valid generic motion peak triggers a normal three-second START_FLUSH
   request for ready producers. A command ACK gates that producer's admission.
4. The feeder sends retained samples oldest-first, catches up and follows live
   acquisition. Sampling never waits for network writes. Window overlap is on Ultra96.
5. A late ready partner starts live on the same capture timeline, without a new
   peak. A partner timeout/disconnect does not stop another usable producer.

A new connection gets a new session/mapping. Initial insufficient history can
fall back once to live-only start for that producer. Losing every usable
producer ends the capture; a fresh peak is required after recovery. Manual
STOP persists across reconnects. AI-confirmed idle stops feeders and rearms
ready nodes so movement can resume capture; acquisition/history continue.
See the [dynamic lifecycle](../../ultra96/ONE_NODE_MODE.md).

## LEDs, rep feedback and fall controls

| State/action | Output |
| --- | --- |
| Connecting or application session lost | Slow LED blink, 500 ms ON / 500 ms OFF. |
| Accepted HELLO_ACK | Three 100 ms connection-confirmation pulses. |
| Connected and waiting/idle | LED OFF after any connection burst. |
| Acknowledged streaming plus activity feedback | Steady LED ON for that node. |
| Accepted phone rep increase | One 100 ms Hip beep; Wrist receives no rep beep. |
| Confirmed idle | LEDs OFF; silent. |
| Qualified enabled Hip fall | Local three-second alarm; separate acknowledged report to server/phone. |

Only Hip detects/reports falls. Final detection starts ON after an ESP reboot;
server terminal `fall on`, `fall off`, `fall status` controls/observes the last
acknowledged setting. Phone reconnect does not change it.

USB commands are `feedback rep`, `feedback active`, `feedback idle`,
`feedback status`, `feedback fall-alarm` and `fall inject`. The first three
test outputs without starting/stopping capture. `feedback fall-alarm` tests
sound only. When enabled, `fall inject` creates a marked synthetic alarm/event;
it is filtered from phone fall alerts and does not validate the detector.
Final firmware does not support the old ready/rep LED or idle-beep patterns.

## Protocol and timing reference

The binary protocol has a 24-byte common header in network byte order.
`PHYSICAL_DATA` adds six float32 SI readings and status (26-byte payload,
50-byte frame). Acquisition assigns timestamp and sequence before buffering.

| Message | ID | Purpose |
| --- | --- | --- |
| HELLO / HELLO_ACK | `0x01` / `0x02` | Identity/capability/session handshake. |
| SYNC_REQ / SYNC_RESP | `0x03` / `0x04` | Four-timestamp clock mapping. |
| ACTIVITY_TRIGGER / START_FLUSH | `0x05` / `0x06` | Motion peak and requested history/live start. |
| STOP / CONTROL_ACK / ARM | `0x08` / `0x09` / `0x0A` | Capture control and acknowledged arming. |
| FEEDBACK_COMMAND / fall setting | `0x0B` / `0x0C` | Activity/rep outputs and explicit Hip detection setting. |
| PHYSICAL_DATA | `0x12` | Acceleration m/s², gyro rad/s and status. |
| DEVICE_EVENT / DEVICE_EVENT_ACK | `0x13` / `0x14` | Separate fall report/receipt. |

Exact field layouts live in [protocol.h](include/protocol.h) and
[protocol.c](src/protocol.c). Mapping subtracts node-minus-Ultra96 offset from
node capture time. Refresh is normally every 60 s; large changes require
confirmation and bounded slew. ACK timeout is normally 3 s for capture controls;
fall-setting commands have a separate 2 s deadline. These settings do not prove
cross-node timing accuracy or end-to-end latency.

## Verification

Use the [portable test guide](test/protocol_host/README.md) and
[measurement procedure](SAMPLING_DIAGNOSTICS.md). Check actual rate/jitter,
axis scaling, history coverage, catch-up capacity, independent joins, outputs,
fall enable/disable and detector-to-phone delivery on deployed devices.
Recorded historical passes are in
[Experiments and results](../../docs/EXPERIMENTS_AND_RESULTS.md).
