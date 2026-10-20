# Two-Node Ultra96 Demo

The active design uses two independent FireBeetle ESP32 boards named Node 1
and Node 2. Each board produces its own local sensor stream and maintains its
own TLS connection through the laptop relay to the Ultra96. There is no
leaf-hub relationship and ESP-NOW forwarding is disabled.

## Identity mapping

| Board | Protocol ID | Assigned source mask | PlatformIO profile |
|---|---:|---:|---|
| Node 1 | `1` | `0x02` | `node1_private_tls` (`node1_mock_tls` without an IMU) |
| Node 2 | `2` | `0x04` | `node2_private_tls` (`node2_mock_tls` without an IMU) |

The assigned masks reserve one source bit per node. They are validated by the
Ultra96 during the `HELLO` handshake.

## One-time configuration

1. Put the DIR-842 2.4 GHz SSID and password only in the ignored
   `include/wifi_secrets.h`.
2. Set `LAPTOP_RELAY_SERVER_IP` in the ignored
   `include/laptop_relay_endpoint_secrets.h` to the laptop's private-LAN
   address, currently `192.168.0.2`. Keep its port at `15000`.
3. Give the laptop adapter on the DIR-842 network the static address
   `192.168.0.2/24`, with no gateway or DNS.
4. Run the concurrent `ultra96/server.py`; each node requires an independent
   server connection and Session ID.
5. Connect one MPU6050 to each ESP32: `SDA -> IO21`, `SCL -> IO22`, `VCC ->
   3.3V`, and `GND -> GND`. Leave AD0 low so the device address is `0x68`.

## Build and flash

From this firmware directory, flash Node 1:

```powershell
pio run -e node1_private_tls -t upload --upload-port COM7
```

Flash Node 2:

```powershell
pio run -e node2_private_tls -t upload --upload-port COM9
```

Change the COM ports if Windows assigns different ones.

## Run the communication path

Start the Ultra96 server, the loopback-only SSH forward, and
`laptop/interactive_hub_relay.py`; then power both boards:

```text
Node 1 --Wi-Fi--\
                  DIR-842 --> laptop:15000 --> SSH tunnel --> Ultra96:5000
Node 2 --Wi-Fi--/
```

The Ultra96 should accept two `HELLO` messages: `node=1` with assigned mask
`0x02`, and `node=2` with assigned mask `0x04`. Both nodes advertise 50 Hz and
receive a different Session ID for each connection. They capture continuously,
but do not send `PHYSICAL_DATA` until Ultra96 commands a flush.

After both HELLO exchanges, the Ultra96 performs five clock-sync rounds with
each node. It forms the candidate offset from the median of the three
lowest-delay usable rounds, rejecting a batch whose selected offsets disagree.
Clock synchronization completes before trigger processing. Either node may
detect the first generic motion peak and send `ACTIVITY_TRIGGER`. Ultra96 maps
the peak into its own clock and then into both ESP32 `esp_timer` domains. It
sends `START_FLUSH` to each node for the preceding five seconds. Each ESP
acknowledges the command, exposes matching history oldest-first, catches up to
the live edge, and then sends every new sample once. Acquisition sequences and
timestamps are assigned before buffering; a live sample cannot overtake an
older historical sample. Ultra96 considers the group flush confirmed only
after both acknowledgements and is the only component that creates overlapping
AI windows.

If either START_FLUSH acknowledgement is still missing after three seconds, the
Ultra96 sends STOP to both nodes, reports the missing Node ID, and disables
trigger handling. This failure is latched until the operator enters `start`.

The Ultra96 refreshes each mapping every 60 seconds. A small consistent change
is applied directly. A change larger than 5 ms is first held as a pending
candidate while an immediate second five-round batch confirms it. The two
candidates must agree within 5 ms before the confirmed midpoint becomes a
target approached at a bounded rate. A delayed, internally inconsistent, or
unconfirmed refresh is rejected without discarding the last working mapping.
Logs distinguish `CLOCK_SYNC_APPLIED`, `CLOCK_SYNC_CONFIRMATION_PENDING`,
`CLOCK_SYNC_LIMITED`, `CLOCK_SYNC_REJECTED`, and
`CLOCK_SYNC_SLEW_COMPLETE`.

A reconnect creates a new Session with no clock mapping: the affected node must
synchronize again. If either node disconnects, the Ultra96 sends STOP to the
surviving node. A later trigger is accepted only after both nodes reconnect and
have fresh mappings. Stale Sessions, stale boot/arm identities, replayed sample
sequences and timestamp regressions are rejected.

### Integration tuning reminder

The initial physical-test defaults use three selected rounds, a 250 ms quality
delay ceiling, a 20 ms selected-offset spread limit, a 5 ms direct-apply limit,
a 5 ms two-batch confirmation agreement limit, and a 1 ms-per-second slew
rate. These are safe starting values for the current Wi-Fi/TLS/laptop-relay
path, not final calibrated constants. During real-IMU and full-team pipeline
integration, record several normal and loaded sync cycles, check Node 1/Node 2
timestamp alignment, then tune the thresholds and slew rate if the evidence
requires it. Keep this item open until that integration test is completed.

Physical trigger handling is armed by default. The Ultra96 server also accepts simple
line-based commands in its terminal:

```text
start   (or s)  arm trigger handling
stop    (or x)  STOP both nodes and disarm trigger handling
```

After a manual STOP, the nodes remain connected and clock synchronization may
continue, but triggers are ignored until a manual START. No additional port,
service, or configuration file is required.

The acknowledgement timeout applies only to confirming START. STOP is already
the safe state, so a missing STOP acknowledgement does not block shutdown or
trigger an automatic restart.

In the interactive relay, press `1` or `2` to select one node, or `a` to
select both for the older diagnostic and benchmark commands.

## Synchronization wire messages

| Type | Direction | Payload |
|---|---|---|
| `SYNC_REQ` (`0x03`) | Ultra96 → ESP | Ultra96 send time `t1` (`u64`, µs) |
| `SYNC_RESP` (`0x04`) | ESP → Ultra96 | `t1`, ESP receive `t2`, ESP send `t3` (three `u64`) |
| `ACTIVITY_TRIGGER` (`0x05`) | ESP → Ultra96 | boot ID (`u32`), arm generation (`u32`), local peak time (`u64`) |
| `START_FLUSH` (`0x06`) | Ultra96 → ESP | common peak time (`u64`), signed ESP-minus-Ultra96 offset (`i64`), history duration (`u32`), trigger node (`u8`), three reserved bytes, trigger sequence (`u32`) |
| `START` (`0x07`) | Ultra96 → ESP | retained legacy common start time (`u64`) and signed ESP-minus-Ultra96 offset (`i64`) |
| `STOP` (`0x08`) | Ultra96 → ESP | empty |
| `CONTROL_ACK` (`0x09`) | ESP → Ultra96 | command type (`u8`), result (`u8`), reserved (`u16`), original command Sequence (`u32`), effective ESP-local time (`u64`) |
| `PHYSICAL_DATA` (`0x12`) | ESP → Ultra96 | `ax,ay,az` float32 m/s², `gx,gy,gz` float32 rad/s, status (`u16`) |

All multibyte values use network byte order. The Ultra96 records receive time
`t4`, estimates round-trip delay as `(t4-t1)-(t3-t2)`, and estimates the ESP
minus Ultra96 offset as `((t2-t1)+(t3-t4))/2`.

SYNC only learns the relationship between the independent clocks; it does not
start or stop sampling. START_FLUSH uses that mapping to translate the one
reported peak into each node's local history boundary. Periodic SYNC refreshes the
mapping so timestamp alignment does not gradually degrade as the clocks drift.

## 50 Hz physical and mock modes

The normal Node 1 and Node 2 profiles now use the local MPU6050 rather than
the mock producer. The firmware verifies `WHO_AM_I`, configures the sensor for
50 Hz, and converts measurements to SI units before storing them. Ultra96
checks its sliding windows against 50 samples per second per node. Battery is
not part of `PHYSICAL_DATA`.

Without MPU6050s, build `node1_mock_tls` and `node2_mock_tls`. These profiles
run the real bounded history, peak detector, feeder, protocol, TLS and Ultra96
paths with deterministic 50 Hz samples and a periodic synthetic motion
excursion. The synthetic peak provides trigger support without a second mock
transport implementation:

```powershell
pio run -e node1_mock_tls
pio run -e node2_mock_tls
```

## Current limitations

- Physical two-board timing accuracy still requires measurement after flashing.
- MPU6050 identity, scale, axis orientation, peak threshold and long-duration
  50 Hz behavior have not been validated on physical sensors.
- The communications target does not import the hardware `StreamCoordinator`;
  Ultra96 owns coordination. The original hardware Bluetooth and ESP-NOW
  activation/mode/LED sources remain unchanged and are not exercised by this
  direct-TLS vertical slice.
- PlatformIO reports a 16 MB configured versus 2 MB detected flash-size
  warning during builds. The build succeeds, but the board flash setting
  should be reconciled before final deployment.
