# Two-Node Ultra96 Demo

The active design uses two independent FireBeetle ESP32 boards named Node 1
and Node 2. Each board produces its own local sensor stream and maintains its
own TLS connection through the laptop relay to the Ultra96. There is no
leaf-hub relationship and ESP-NOW forwarding is disabled.

## Identity mapping

| Board | Protocol ID | Assigned source mask | PlatformIO profile |
|---|---:|---:|---|
| Node 1 | `1` | `0x02` | `node1_private_tls` |
| Node 2 | `2` | `0x04` | `node2_private_tls` |

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
`0x02`, and `node=2` with assigned mask `0x04`. Both DATA streams should run
concurrently with a different Session ID for each connection.

After both HELLO exchanges, the Ultra96 performs five clock-sync rounds with
each node. It forms the candidate offset from the median of the three
lowest-delay usable rounds, rejecting a batch whose selected offsets disagree.
When both mappings are ready, it schedules one common START four seconds into
the future. Each ESP converts that coordinator time to its local `esp_timer`
clock, begins sampling at the scheduled instant, and resets its DATA Sequence
to zero. Each ESP immediately returns `CONTROL_ACK` after it has successfully
armed that local start. The Ultra96 considers the group START confirmed only
after both acknowledgements.
If either START acknowledgement is still missing after three seconds, the
Ultra96 sends STOP to both nodes, reports the missing Node ID, and disables
automatic restart. The extra one-second margin lets STOP cancel an armed start
before its scheduled sampling time. This failure is latched until the operator
enters `start`.

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
surviving node; once both nodes are connected and synchronized again, it issues
a new common START.

### Integration tuning reminder

The initial physical-test defaults use three selected rounds, a 250 ms quality
delay ceiling, a 20 ms selected-offset spread limit, a 5 ms direct-apply limit,
a 5 ms two-batch confirmation agreement limit, and a 1 ms-per-second slew
rate. These are safe starting values for the current Wi-Fi/TLS/laptop-relay
path, not final calibrated constants. During real-IMU and full-team pipeline
integration, record several normal and loaded sync cycles, check Node 1/Node 2
timestamp alignment, then tune the thresholds and slew rate if the evidence
requires it. Keep this item open until that integration test is completed.

Automatic START is enabled by default. The Ultra96 server also accepts simple
line-based commands in its terminal:

```text
start   (or s)  request a synchronized START
stop    (or x)  STOP both nodes and suppress automatic restart
```

After a manual STOP, the nodes remain connected and clock synchronization may
continue, but DATA stays stopped until a manual START. If START is entered
before both nodes are ready, Ultra96 remembers it and schedules START as soon
as both clock mappings are available. No additional port, service, or
configuration file is required.

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
| `START` (`0x07`) | Ultra96 → ESP | common Ultra96 start time (`u64`) and signed ESP-minus-Ultra96 offset (`i64`) |
| `STOP` (`0x08`) | Ultra96 → ESP | empty |
| `CONTROL_ACK` (`0x09`) | ESP → Ultra96 | command type (`u8`), result (`u8`), reserved (`u16`), original command Sequence (`u32`), effective ESP-local time (`u64`) |

All multibyte values use network byte order. The Ultra96 records receive time
`t4`, estimates round-trip delay as `(t4-t1)-(t3-t2)`, and estimates the ESP
minus Ultra96 offset as `((t2-t1)+(t3-t4))/2`.

SYNC only learns the relationship between the independent clocks; it does not
start or stop sampling. START uses that mapping to translate one Ultra96 time
into the correct local deadline on each ESP. Periodic SYNC refreshes the
mapping so timestamp alignment does not gradually degrade as the clocks drift.

## 100 Hz physical smoke test

The normal Node 1 and Node 2 profiles now use the local MPU6050 rather than
the mock producer. The firmware verifies `WHO_AM_I`, configures the sensor for
100 Hz, and prints one compact live sample per second on each serial monitor.
The Ultra96 prints received DATA and checks its sliding windows against 100
samples per second per node. Raw signed accelerometer and gyroscope counts are
sent unchanged; calibration and conversion to physical units are intentionally
outside this first streaming test.

## Current limitations

- Physical two-board timing accuracy still requires measurement after flashing.
- PlatformIO reports a 16 MB configured versus 2 MB detected flash-size
  warning during builds. The build succeeds, but the board flash setting
  should be reconciled before final deployment.
