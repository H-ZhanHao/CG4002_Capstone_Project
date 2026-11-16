# FitSense communication protocol — demo revision guide

Source checked: 13 November 2026. This guide describes the communications firmware in `communications/firmware/FireBeetle_to_Ultra96`, the integrated AI server `server/server_rep_fall.py`, and the Unity visualizer protocol. It describes checked-in behavior; the actual boards and server must run matching builds/settings.

## 1. Facts to remember first

| Item | Current implementation |
| --- | --- |
| Wearables | Two independent ESP32 producers: Node 1 = Hip, Node 2 = Wrist |
| Sensor interface | MPU6050 to ESP32 over I²C |
| Physical sampling rate | 100 Hz per node, approximately one sample every 10 ms |
| Physical sample frame | **50 bytes = 24-byte header + 26-byte payload** |
| Sensor network format | Custom version-1 binary application protocol |
| Sensor route | Private 2.4 GHz Wi-Fi → laptop relay → SSH tunnel → Ultra96 |
| AR route | Bidirectional JSON messages over secure WebSocket (WSS), through the laptop relay/tunnel |
| Integrated AI server defaults | 3-second sliding window, 1-second hop |
| Rep ownership | Phone supplies repetition events; the server validates and routes feedback |
| Rep sound | One accepted count increase requests one 100 ms beep on Hip only |
| Activity feedback | LED ON for active streaming, OFF for idle/stopped state; idle is silent |
| Fall control | Final firmware starts detection OFF at reboot; server terminal enables/disables it on Hip |

The firmware uses the name Hip for Node 1. Use the actual worn location consistently when describing the thigh/hip sensor in your presentation.

## 2. Protocols on each link

| Link | Protocol | Information |
| --- | --- | --- |
| MPU6050 ↔ ESP32 | I²C | Acceleration and angular velocity on three axes each |
| ESP32 ↔ laptop interactive relay | TLS over TCP/IP over private Wi-Fi | Binary IMU frames, connection setup, synchronization, capture controls, feedback and device events |
| Laptop ↔ Ultra96 | TCP forwarded through SSH | Binary sensor/control traffic; also a separate forwarded connection for the AR gateway |
| Android ↔ Ultra96 visualizer gateway, via laptop | JSON over WSS (WebSocket over TLS/TCP) | Classifications, rep session messages, feedback receipts and fall notifications |
| Ultra96 AI server ↔ gateway, on the same board | Loopback TCP with newline-delimited JSON | Classification publishing on port 9100; rep/fall control on port 9101 |

### Where encryption ends

For the **interactive wearable relay** documented in `DEMO_TWO_NODES.md`, ESP32 TLS terminates at `interactive_hub_relay.py` on the laptop. The laptop can parse/inject binary protocol messages. The onward connection is protected by the SSH tunnel to Ultra96.

For the **AR route**, TLS terminates at `visualizer_gateway.py` on Ultra96. The phone-facing `tcp_relay.py` copies encrypted bytes without decrypting them. The gateway certificate/private key belongs on Ultra96.

`tcp_relay.py` itself does not add TLS. An opaque relay preserves whatever protocol its endpoints establish; end-to-end wearable TLS would require a TLS endpoint upstream. Do not describe the interactive wearable path as one uninterrupted ESP32-to-Ultra96 TLS session.

### Typical ports in the documented setup

| Service | Port/path |
| --- | --- |
| Wearable-facing laptop relay | Laptop TCP 15000 |
| Laptop local SSH forward for sensors | `127.0.0.1:15001` → Ultra96 `127.0.0.1:5000` |
| Phone-facing laptop relay | Laptop TCP 9000 |
| Laptop local SSH forward for AR | `127.0.0.1:19000` → Ultra96 `127.0.0.1:9000` |
| Ultra96 classification publisher | Loopback TCP 9100 |
| Ultra96 rep/fall control service | Loopback TCP 9101 |

These are setup values, not fields embedded in the application packet. Ports 9100/9101 are internal services, not direct phone endpoints.

## 3. Explain the operating sequence

1. **Connect and identify.** Each ESP32 joins the private Wi-Fi and establishes its own TLS/TCP connection to the laptop relay. `HELLO` carries identity, firmware version, boot ID, capabilities, source mask and a device token. Ultra96 validates it and sends `HELLO_ACK`, assigning a nonzero transport session ID.
2. **Synchronize clocks.** Ultra96 exchanges `SYNC_REQ`/`SYNC_RESP` with each node and estimates the offset between the node's monotonic clock and the server's monotonic clock.
3. **Arm activity detection.** The server sends `ARM`; the node acknowledges it. Local 100 Hz sampling maintains a short history while the node waits for movement.
4. **Trigger and recover history.** An armed node detects a generic movement peak and sends `ACTIVITY_TRIGGER`. This is an activity trigger, not an exercise classification. The server maps the peak timestamp to its timeline and uses `START_FLUSH` to request the historical interval and continued live data. The normal history duration is 3 seconds; the exact available coverage is validated.
5. **Stream and classify.** A successful start acknowledgement allows that node's data into the capture pipeline. The integrated server defaults to 3-second windows with a 1-second hop, then invokes the DPU-accelerated model.
6. **Update AR.** Ultra96 publishes `exercise_update`; the gateway sends it over WSS so the phone can select the corresponding exercise/minigame.
7. **Return repetitions.** The phone starts/synchronizes a rep session, then sends cumulative `rep_event` updates. The server validates them and requests Hip buzzer feedback using `FEEDBACK_COMMAND`. `CONTROL_ACK` records firmware acceptance; the phone receives rep result/delivery messages.
8. **Handle idle and falls.** Confirmed AI idle pauses capture and clears activity LEDs. Fall reporting is a separate local-device event path and can operate while exercise capture is stopped, provided fall detection has been enabled.

Each node can synchronize/start independently, and a late node can join the existing timeline. Communication support for a single node does not by itself prove the deployed AI model's single-node accuracy or compatibility.

## 4. Binary frame format

All binary application frames use a **24-byte fixed header**, followed by a message-specific payload. Multi-byte fields use **network byte order (big-endian)**. Python header format: `!HBBBBHIIQ`.

| Byte offset | Header field | Bytes | Purpose |
| --- | --- | ---: | --- |
| 0–1 | Magic | 2 | `0xA55A`, identifies the protocol |
| 2 | Version | 1 | Currently `1` |
| 3 | Message type | 1 | Identifies the payload/operation |
| 4 | Node ID | 1 | Producer identity; server-originated commands normally use `0` |
| 5 | Flags | 1 | Header flags; validated per message |
| 6–7 | Payload length | 2 | Bytes following the header |
| 8–11 | Session ID | 4 | Current transport session |
| 12–15 | Sequence number | 4 | Ordering/correlation according to the message type |
| 16–23 | Timestamp | 8 | Microseconds; interpretation depends on message type/direction |
| | **Header total** | **24** | |

TCP is a byte stream: one read may contain a partial frame or several frames. Receivers accumulate bytes, parse the header, then wait for the declared payload length. TCP packet boundaries are not application frame boundaries.

### Physical IMU sample: `PHYSICAL_DATA` (`0x12`)

Python payload format: `!ffffffH`.

| Frame byte offset | Payload field | Bytes |
| --- | --- | ---: |
| 24–27 | `ax` | 4 |
| 28–31 | `ay` | 4 |
| 32–35 | `az` | 4 |
| 36–39 | `gx` | 4 |
| 40–43 | `gy` | 4 |
| 44–47 | `gz` | 4 |
| 48–49 | Status bitmask | 2 |
| | **Payload total** | **26** |

The six values are 32-bit floats in physical units: acceleration in m/s² and angular velocity in rad/s. Sequence and capture timestamp are in the header. Example status bits: `0x0001` sensor valid, `0x0002` historical sample, `0x0004` history overwrite.

**Size calculation:** 24 + (6 × 4) + 2 = **50 bytes per physical sample frame**.

At 100 Hz during live streaming:

- One node: 50 × 100 = **5,000 application bytes/second**, or 40 kbit/s.
- Two nodes: **10,000 application bytes/second**, or 80 kbit/s.

These figures exclude control traffic, historical bursts, TLS, TCP/IP, SSH and Wi-Fi overhead. They are not measured radio throughput.

The older/test `DATA` (`0x10`) uses six signed 16-bit values plus 2-byte status: **14-byte payload, 38-byte frame**. It must not be confused with the **50-byte physical-data frame** used by physical integration.

## 5. Binary packet types

Sizes below include the common 24-byte header. “Node” means the relevant independent ESP32, not an ESP-NOW leaf/hub. The following types are implemented; not all occur in every exercise session.

### Connection, clock and capture

| Code | Type | Direction | Payload / total bytes | Purpose |
| --- | --- | --- | --- | --- |
| `0x01` | `HELLO` | Node → server | 44 / 68 | Identity, capabilities and token validation |
| `0x02` | `HELLO_ACK` | Server → node | 0 / 24 | Accept connection; assign transport session |
| `0x03` | `SYNC_REQ` | Server → node | 8 / 32 | Server send timestamp `t1` |
| `0x04` | `SYNC_RESP` | Node → server | 24 / 48 | Echo `t1`, node receive `t2`, node send `t3` |
| `0x05` | `ACTIVITY_TRIGGER` | Node → server | 16 / 40 | Boot ID, arm generation and detected peak timestamp |
| `0x06` | `START_FLUSH` | Server → node | 28 / 52 | Start at a mapped historical boundary, flush history, continue live |
| `0x07` | `START` | Server → node | 16 / 40 | Scheduled start control, also used by other supported capture paths |
| `0x08` | `STOP` | Server → node | 0 / 24 | Stop capture/streaming for the applicable control state |
| `0x09` | `CONTROL_ACK` | Node → server | 16 / 40 | Acknowledge command type, result, command sequence and effective local time |
| `0x0A` | `ARM` | Server → node | 20 / 44 | Scheduled arm boundary, clock offset and arm generation |

### Samples, outputs and fall events

| Code | Type | Direction | Payload / total bytes | Purpose |
| --- | --- | --- | --- | --- |
| `0x0B` | `FEEDBACK_COMMAND` | Server → node | 16 / 40 | Event ID plus buzzer/activity action |
| `0x0C` | `FALL_DETECTION_SET` | Server → Hip | 4 / 28 | Explicit enable/disable setting; acknowledged with `CONTROL_ACK` |
| `0x10` | `DATA` | Node → server | 14 / 38 | Raw/test integer sample format |
| `0x11` | `HEARTBEAT` | Node → server | 6 / 30 | Status and overflow count when normal sample traffic is absent |
| `0x12` | `PHYSICAL_DATA` | Node → server | 26 / 50 | Real six-axis floating-point physical sample |
| `0x13` | `DEVICE_EVENT` | Hip → server | 32 / 56 | Fall event identity, occurrence time, alarm duration and synthetic marker |
| `0x14` | `DEVICE_EVENT_ACK` | Server → Hip | 16 / 40 | Accept/duplicate receipt for a device event |
| `0x7F` | `ERROR` | Receiver → sender | 4 / 28 | Error code and offending message type |

### Diagnostic and assessment features

These support protocol demonstrations/tests; they are not the normal physical exercise stream. The interactive laptop relay can originate diagnostic requests.

| Code | Type | Direction | Payload / total bytes | Purpose |
| --- | --- | --- | --- | --- |
| `0x20` | `TEST_REQUEST` | Controller → node | 14 / 38 | Request/response protocol test |
| `0x21` | `TEST_RESPONSE` | Node → controller/server | 14 / 38 | Return test response |
| `0x22` | `BENCHMARK_CONTROL` | Controller → node | 8 / 32 | Start/stop a bounded throughput benchmark |
| `0x23` | `BENCHMARK_REPORT` | Node → controller/server | 24 / 48 | Counts, elapsed time, application bytes, drops and failures |
| `0x30` | `FILE_BEGIN` | Sender → receiver | 44 / 68 | File identity, size, chunk size and expected SHA-256 |
| `0x31` | `FILE_CHUNK` | Sender → receiver | 16 + N / 40 + N | Chunk metadata/CRC and N data bytes; N ≤ 1008 |
| `0x32` | `FILE_END` | Sender → receiver | 40 / 64 | Complete transfer with final metadata/digest |
| `0x33` | `FILE_RESULT` | Receiver → sender | 44 / 68 | Report transfer result and received digest/size |

Maximum payload: **1024 bytes**. Maximum binary application frame: **1048 bytes**. File chunks use CRC validation and whole-file SHA-256 validation; do not claim every IMU frame contains an application CRC.

### Feedback payload and acknowledgement

`FEEDBACK_COMMAND` payload is `!QB7x`: 8-byte event ID, 1-byte action, 7 reserved zero bytes.

| Action | Value | Normal destination/effect |
| --- | ---: | --- |
| `REP_BUZZ` | 4 | Hip only; one rep confirmation beep |
| `ACTIVITY_ACTIVE` | 5 | Activity LED ON on participating connected nodes |
| `ACTIVITY_IDLE` | 6 | Activity LED OFF; no idle sound |

Old actions 1/2/3 are retired. `CONTROL_ACK` payload is `!BBHIQ`: command type, result, reserved bytes, original command sequence and effective local timestamp. Result 0 means applied; 3 means duplicate. Other results can report invalid state, insufficient history, stale identity, busy, unsupported or conflict depending on the firmware/control path.

Feedback retries retain the same event/sequence/frame, at 750 ms intervals, with at most 3 send attempts and a 2.5-second deadline. Duplicates must not replay the physical feedback. An ACK confirms firmware acceptance, **not a physical measurement that the LED or buzzer worked**.

## 6. Clock synchronization: simple explanation

Four timestamps are used:

- `t1`: Ultra96 sends request.
- `t2`: ESP32 receives request.
- `t3`: ESP32 sends reply.
- `t4`: Ultra96 receives reply; measured locally by Ultra96.

Estimated node-minus-server offset:

```text
offset ≈ ((t2 − t1) + (t3 − t4)) / 2
network round trip ≈ (t4 − t1) − (t3 − t2)
server-time estimate of node sample = node timestamp − offset
```

The server targets five rounds, uses the three lowest-delay usable rounds and their median offset, and rejects poor-quality/inconsistent results. Periodic refresh is configured at 60 seconds; actual completion depends on successful exchanges. Mapping is scoped to the node's transport session.

Presentation wording: “We estimate the offset between each wearable clock and Ultra96, then translate capture timestamps onto one common timeline. We use several exchanges and reject inconsistent measurements.”

This is an NTP-style estimate, not a claim of perfect hardware clock synchronization or guaranteed zero timestamp error.

## 7. AR JSON protocol: all message types

Each external WSS text message contains one JSON object. JSON is variable length; AR messages do **not** carry the wearable's 24-byte binary header. Internal loopback links use newline-delimited JSON.

| Type | Direction | Meaning |
| --- | --- | --- |
| `exercise_update` | Ultra96 → phone | Current exercise classification/state |
| `rep_welcome` | Ultra96 → phone | Current server run ID and requirement to establish/sync rep state |
| `rep_session_start` | Phone → Ultra96 | Establish a new exercise set and baseline count |
| `rep_sync` | Phone → Ultra96 | Synchronize an existing set/count, including after reconnect |
| `rep_event` | Phone → Ultra96 | Report a new cumulative repetition count |
| `rep_result` | Ultra96 → phone | Request accepted, rejected, duplicated, session started or synchronized |
| `rep_delivery` | Ultra96 → phone | Later feedback delivery outcome when an asynchronous receipt is needed |
| `fall_event` | Ultra96 → phone | Fall notification from Hip |
| `fall_ack` | Phone → Ultra96 | Client acceptance of that fall identity |

The examples below use illustrative values and matching identities. They are not captured traffic or values to hard-code.

### 7.1 Exercise update

```json
{
  "v": 1,
  "type": "exercise_update",
  "run_id": 12345678,
  "seq": 12,
  "ts_us": 987654321,
  "exercise": "squat"
}
```

- `v`: JSON protocol version.
- `run_id`: identifies the server publisher run, separating updates across restarts.
- `seq`: orders classification updates within that run.
- `ts_us`: Ultra96 monotonic publication timestamp in microseconds, not a wall-clock date.
- `exercise`: exercise/state string. Six exercise labels are `squat`, `push_up`, `sit_up`, `lunge`, `leg_raise`, `jumping_jack`. The integrated AI also uses `idle`; testing/unknown state may use `unknown`.

These six fields form the stable exercise-update contract. Extra metadata is allowed by that validator; the current minimal publisher does not require confidence or repetition count fields. Phone rep requests accept only the six exercise labels, not `idle` or `unknown`.

### 7.2 Rep welcome

```json
{
  "v": 1,
  "type": "rep_welcome",
  "run_id": 12345678,
  "requires_sync": true
}
```

The phone must use this run ID and establish/synchronize its rep state before new rep events. A server restart requires a new session rather than reusing stale run state.

### 7.3 Start an exercise set

```json
{
  "v": 1,
  "type": "rep_session_start",
  "run_id": 12345678,
  "seq": 1,
  "rep_session_id": "squat_set_01",
  "rep_index": 0,
  "exercise": "squat"
}
```

This establishes a new baseline without a rep beep. `rep_session_id` identifies the exercise set, separately from the binary transport session ID. It must be 1–64 letters, digits, underscores or hyphens. Start a fresh set with a fresh ID when resetting the counter.

### 7.4 Report a repetition

```json
{
  "v": 1,
  "type": "rep_event",
  "run_id": 12345678,
  "seq": 2,
  "rep_session_id": "squat_set_01",
  "rep_index": 1,
  "exercise": "squat"
}
```

`rep_index` is the **cumulative count**, not an increment command. `seq` orders phone requests on the current link; it is separate from classification `seq` and binary frame sequences. The server checks run ID, session, exercise, sequence and count before requesting feedback.

An increase from 3 to 5 produces one current confirmation beep, not two replayed beeps. A repeated count does not produce another beep.

### 7.5 Synchronize a set after reconnect

```json
{
  "v": 1,
  "type": "rep_sync",
  "run_id": 12345678,
  "seq": 3,
  "rep_session_id": "squat_set_01",
  "rep_index": 1,
  "exercise": "squat"
}
```

Synchronization restores a baseline without beeping. The count cannot move backwards; resetting requires a new set. The same seven keys shown above are required for all three phone rep request types; unexpected fields are rejected.

### 7.6 Immediate rep result

```json
{
  "v": 1,
  "type": "rep_result",
  "run_id": 12345678,
  "seq": 2,
  "rep_session_id": "squat_set_01",
  "rep_index": 1,
  "status": "accepted",
  "reason": "new_rep",
  "feedback_event_id": 42,
  "nodes": [
    { "node_id": 1, "status": "pending", "result": -1 }
  ]
}
```

This acknowledges the phone request. `accepted` does not mean the wearable has already acknowledged it. `feedback_event_id` correlates the server's feedback work; `nodes` reports per-node delivery state. `-1` means no firmware result is available.

Other result statuses include `session_started`, `synced`, `duplicate` and `rejected`. Baseline start/sync responses have no new feedback event (`feedback_event_id: 0`, `nodes: []`).

### 7.7 Later rep delivery report

```json
{
  "v": 1,
  "type": "rep_delivery",
  "run_id": 12345678,
  "seq": 2,
  "rep_session_id": "squat_set_01",
  "rep_index": 1,
  "status": "complete",
  "reason": "new_rep",
  "feedback_event_id": 42,
  "nodes": [
    { "node_id": 1, "status": "applied", "result": 0 }
  ]
}
```

When the initial response contains pending delivery, a later message reports its outcome. Top-level `complete` means the delivery observation has finished; inspect `nodes` to determine success. Outcomes can include `applied`, `timeout`, `rejected`, `send_failed`, `disconnected`, `cancelled` or `unavailable`. A separate delivery report is unnecessary if the initial response already contains the final state.

### 7.8 Fall event

```json
{
  "v": 1,
  "type": "fall_event",
  "node_id": 1,
  "boot_id": 12345,
  "event_id": "42"
}
```

Identity is `(node_id, boot_id, event_id)`, independent of exercise runs and rep sessions. `boot_id` is a positive uint32 JSON integer. **Fall `event_id` is a positive uint64 encoded as a decimal string**, preserving its integer value across JSON consumers. It is different from numeric rep `feedback_event_id`.

The wire message deliberately omits occurrence timestamp, alarm duration and synthetic marker. Synthetic test injections are acknowledged internally but filtered from AR alerts.

### 7.9 Fall acknowledgement

```json
{
  "v": 1,
  "type": "fall_ack",
  "node_id": 1,
  "boot_id": 12345,
  "event_id": "42"
}
```

The phone echoes the event identity and changes `type`. This means the client accepted the event; it does not mean the user dismissed the popup or that a person is safe. Duplicate fall notifications are acknowledged without opening a fresh alert each time. Fall messages require exactly these five fields.

## 8. End-to-end rep and fall paths

### Rep

```text
Phone produces cumulative rep count
  → rep_event JSON over WSS
  → Ultra96 validates run/session/sequence/count
  → FEEDBACK_COMMAND (0x0B, action 4) to Hip
  → Hip accepts and queues one beep
  → CONTROL_ACK (0x09)
  → rep_result / rep_delivery JSON to phone
```

The protocol transports the phone-owned count; it does not implement the AR repetition counting algorithm.

### Fall

```text
Hip detects fall locally, when enabled, and starts local alarm
  → DEVICE_EVENT (0x13)
  → Ultra96 validates and deduplicates
  → DEVICE_EVENT_ACK (0x14) to Hip
  → fall_event JSON over existing WSS connection
  → phone accepts event and shows alert
  → fall_ack JSON to Ultra96
```

Device receipt and AR receipt are separate acknowledgements. The local alarm does not wait for network delivery. Both pending delivery and duplicate history are bounded and kept in memory, not durable storage.

Hip retains up to 8 pending event reports for 30 seconds, retrying every 500 ms. Ultra96 separately retains up to 32 pending AR fall reports for 30 seconds, retrying no faster than once per second per event. Unacknowledged AR events can be replayed on reconnect within that lifetime.

For `node1_final`, enable detection using the running server terminal:

```text
fall on
fall status
fall off
```

The setting uses `FALL_DETECTION_SET` (`0x0C`) with `!B3x`: a 0/1 enable byte and three reserved zero bytes. Only Hip accepts it. The server waits for an ACK; it does not automatically replay a setting after reconnect. The AR client has no fall-detection toggle command.

## 9. Reliability and security answers

| Mechanism | What it gives us | Limit to explain accurately |
| --- | --- | --- |
| TCP | Reliable, ordered bytes on a live connection | No message boundaries or automatic recovery of application state across reconnect |
| TLS | Encryption/integrity and certificate-based server identity checking | Wearable TLS ends at the interactive laptop relay; AR TLS ends at Ultra96 |
| SSH tunnel | Protects forwarded laptop-to-Ultra96 traffic | Does not replace the application frame/session checks |
| HELLO/device token | Application-level identity/capability checks | Current token mechanism is demo-level shared-token authentication, not per-device mutual TLS |
| Transport session ID | Rejects stale traffic from another connection | Different from a phone rep-session ID, boot ID and publisher run ID |
| Sequence/event IDs | Correlates requests and detects stale/conflicting/duplicate work | Semantics vary by message type; do not promise global exactly-once delivery |
| HEARTBEAT/timeouts | Reports idle source state and detects inactive connections | A heartbeat does not prove healthy IMU data or accurate inference |
| Bounded queues/retries | Limits memory/backlog and retry duration | Data/events can expire or be dropped; guarantees are bounded |
| Latest-value AR updates | Reconnecting phone gets current classification; stale unsent classifications are replaced | Not an archive of every classification |
| Separate receipt queue | Rep/fall receipts are not replaced by newer exercise classifications | Still bounded, with reconnect/error handling |

Node reconnect uses exponential backoff from 1 second up to 30 seconds and performs a fresh handshake/session. When normal sample traffic is absent, heartbeat scheduling is approximately once per second. The server's client inactivity timeout is 3 seconds.

The gateway accepts one phone, limits incoming phone messages to 1024 bytes and 20 messages/second, and accepts only the supported rep requests or `fall_ack`. Its configurable classification delivery ceiling defaults to 20 Hz; this is not the AI inference rate or the 100 Hz sensor rate.

## 10. Slide-ready summary

**Communication protocol**

- **Sensor interface:** I²C, six-axis motion capture at 100 Hz.
- **Wearable link:** custom binary frames over TLS/TCP on private Wi-Fi.
- **Relay link:** TCP forwarding through an SSH tunnel to Ultra96.
- **AR link:** bidirectional JSON over secure WebSocket.
- **Physical sample frame:** 24-byte header + 26-byte payload = **50 bytes**.
- **Coordination:** HELLO/session setup → clock sync → ARM → activity trigger → historical flush/live stream.
- **Feedback:** validated phone rep events → acknowledged Hip buzzer command; activity state controls LEDs.
- **Reliability:** session/sequence validation, heartbeats, bounded retries and duplicate suppression.

For the architecture diagram, use **100 Hz** and **3-second windows / 1-second hop** for the integrated server defaults. Include phone-to-server repetition events and server-to-wearable capture/feedback commands. Classification results go to the phone; wearables receive output/control commands.

## 11. A one-minute speaking script

“Each wearable reads acceleration and angular velocity from its MPU6050 over I²C at 100 hertz. We package each physical sample into a 50-byte binary frame, including its node identity, session, sequence and timestamp.

“The wearables connect over TLS-protected TCP on our private Wi-Fi. The interactive laptop relay terminates this TLS connection and forwards the protocol to Ultra96 through an SSH tunnel.

“After a handshake, Ultra96 estimates each node's clock offset and arms activity detection. A movement trigger starts historical-data flushing and live streaming. Our integrated server defaults to three-second windows with a one-second hop for AI classification.

“Classification results reach the Android AR app as JSON over secure WebSocket. The app sends repetition events back through the same connection. The server validates those events and sends acknowledged feedback commands to the Hip node. Falls use a separate acknowledged event path, so local detection and alarms do not depend on the AR connection.”

## 12. Likely presentation questions

**How big is one physical data packet?** 50 application bytes: 24-byte header, six 4-byte floats, 2-byte status. Network/encryption overhead is additional.

**Why binary for sensors but JSON for AR?** Binary provides a fixed, compact layout for frequent samples. JSON makes less frequent exercise/events easy to validate, inspect and integrate with Unity.

**Why TCP?** Ordered, reliable transport simplifies sample/control delivery. Session and sequence checks remain necessary for application state, correlation and reconnect behavior.

**Why clock synchronization if we already have timestamps?** Each ESP32 starts with a different local clock. Offset estimates let Ultra96 compare capture times and assemble windows on a common timeline, rather than using variable network arrival times.

**Does the laptop classify exercises?** In this architecture, inference runs on Ultra96. The laptop relays network traffic and can inject diagnostic commands in interactive mode.

**Does the phone receive raw IMU streams?** The current AR protocol carries classifications, rep control/receipts and fall events. Raw IMU samples go to Ultra96.

**Does every rep beep both nodes?** No. Phone-triggered rep feedback targets Hip only. Activity LED feedback applies to the connected participating nodes.

**What prevents a duplicate beep?** Phone run/session/sequence/count validation and wearable feedback event/sequence duplicate handling. Retry frames preserve the same identity.

**Does a successful rep response mean the buzzer sounded?** Initial acceptance validates the request. The wearable ACK confirms firmware accepted the command. Neither is a physical sound measurement.

**Is a fall ACK the same as the user dismissing the alert?** No. It confirms client acceptance of the event. Popup dismissal is local UI behavior.

**Do all packets have a CRC?** No application CRC is appended to every IMU frame. TLS provides transport integrity; the file-transfer feature has chunk CRC and whole-file SHA-256 checks.

**Are the clocks perfectly synchronized?** No. This is an NTP-style offset estimate with quality checks and refresh, subject to network-delay uncertainty.

**Is there ESP-NOW forwarding between the two nodes?** Not in the selected independent-node communications firmware. Each has its own connection. The separate top-level hardware project contains Bluetooth/ESP-NOW functionality and must not be mixed into this network-path explanation.

## 13. Code references for revision

- [Binary types, constants and payload sizes](communications/firmware/FireBeetle_to_Ultra96/include/protocol.h)
- [Binary serialization](communications/firmware/FireBeetle_to_Ultra96/src/protocol.c)
- [Node network transport, TLS and heartbeats](communications/firmware/FireBeetle_to_Ultra96/src/tcp_client.c)
- [Two-node route and TLS-terminating relay](communications/firmware/FireBeetle_to_Ultra96/DEMO_TWO_NODES.md)
- [Interactive laptop relay](communications/laptop/interactive_hub_relay.py)
- [Opaque laptop relay and phone route](communications/laptop/README.md)
- [Integrated AI server and window defaults](server/server_rep_fall.py)
- [AI window/model configuration](server/config.py)
- [Gateway WSS and routing](communications/ultra96/visualizer_gateway.py)
- [Exercise and phone rep schemas](communications/ultra96/visualizer_protocol.py)
- [Rep session validation/results/delivery](communications/ultra96/rep_control.py)
- [Feedback actions, retries and statuses](communications/ultra96/feedback_delivery.py)
- [Binary fall receipt/ACK](communications/ultra96/device_events.py)
- [Fall JSON validation and retry outbox](communications/ultra96/fall_delivery.py)
- [Final firmware fall settings](server/FINAL_FALL_CONTROL.md)
- [Unity rep JSON builder/parser](communications/unity/Ultra96VisualizerDemo/Assets/Scripts/Networking/RepFeedbackProtocol.cs)
- [Unity fall JSON parser/ACK builder](communications/unity/Ultra96VisualizerDemo/Assets/Scripts/Networking/FallProtocol.cs)

When answering implementation questions, prefer the encoder/decoder and active integrated server over old phase notes. Some legacy comments still describe 50 Hz, 5-second windows, earlier feedback actions or a leaf/hub topology.
