# Communications: AI and Unity integration brief

This is a short explanation of my communication components, their runtime behaviour, and where our components connect. It describes the current source code; real IMU/AI integration is still pending.

## 1. Overall workflow

```text
Node 1 (ESP32) ─┐
               ├─ private Wi-Fi → laptop relay → SSH tunnel → Ultra96 server.py
Node 2 (ESP32) ─┘                                            │
                                                    [AI input hook to add]
                                                            ↓
                                                     AI inference process
                                                            │ JSON + newline
                                                            ↓
                                             Ultra96 127.0.0.1:9100
                                                  visualizer_gateway.py
                                                            │ WSS
                                                            ↓
                                        SSH tunnel → laptop relay → Unity phone
```

The laptop bridges the private router network and the Ultra96 route through VPN/SSH. Each ESP32 has its own connection and produces its own samples. The active setup uses **Node 1 and Node 2**, with no ESP-NOW leaf/hub forwarding. Older documents mentioning Hub 3/Hub 6 describe the previous demo topology.

## 2. What each program does

| Component | Behaviour |
|---|---|
| ESP32 firmware | Continuously captures a local MPU6050 at 50 Hz into bounded history, detects a generic peak, and flushes history before live `PHYSICAL_DATA`. Handles handshake, clock sync, START_FLUSH/STOP, status and reconnects. |
| `laptop/tcp_relay.py` | Copies bytes in both directions, with an independent upstream connection per client. Used separately for sensor traffic and phone traffic. Does not interpret messages or terminate TLS. |
| `laptop/interactive_hub_relay.py` | Alternative sensor relay for keyboard-triggered tests, benchmarks and file transfers. This version terminates ESP32 TLS on the laptop before forwarding protocol frames through SSH. |
| `ultra96/server.py` | Handles nodes concurrently, authenticates HELLO, assigns sessions, validates/decodes sensor frames, tracks stream health, synchronizes clocks, coordinates capture, and feeds a shared neutral sliding-window assembler. It does not yet feed an AI model. |
| `ultra96/visualizer_gateway.py` | Separate process accepting local inference JSON and sending it to one Unity client over WSS. Validates messages, keeps the latest state and limits outgoing updates to 20 Hz by default. |
| Unity networking scripts | Connect over pinned WSS, validate messages, reject duplicate/older updates within a run, reconnect automatically and expose updates to the Unity main thread. Current scene displays a debug panel. |

The sensor connection enters Ultra96 at port **5000**. The gateway listens on **9000** for WSS and **9100** for local JSON publication; both gateway listeners bind to Ultra96 loopback.

## 3. Sensor startup, timing and recovery

1. Each node connects through the laptop and sends HELLO. Ultra96 validates its identity and allocates a fresh Session ID.
2. Ultra96 performs five clock-sync exchanges per node and estimates each node's clock offset using the three lowest-delay usable rounds.
3. Once both nodes have fresh clock mappings, either node may report the first generic peak with `ACTIVITY_TRIGGER`. Ultra96 maps that peak and sends `START_FLUSH` to both nodes.
4. Each node normally produces one sample every 20 ms. It sends the preceding five seconds oldest-first, catches up to the live edge, and then sends each new sample once. Capture timestamps and sequences are assigned at acquisition and preserved through buffering.
5. Ultra96 refreshes clock mappings every 60 seconds. It applies small consistent corrections directly, slews toward larger ones and rejects poor measurements while retaining the previous mapping.

If either START_FLUSH acknowledgement is missing after three seconds, Ultra96 sends STOP to both and requires a manual `start`. The server terminal also accepts `stop` to disarm trigger handling and `start` to re-arm it when both nodes are ready. Continuous bounded acquisition remains separate from transmission.

If a node disconnects, Ultra96 stops the surviving node. Reconnection requires a new session and synchronization before another common START. Firmware uses bounded buffering and reconnect backoff; delivery should not be treated as lossless through an outage. AI must handle sequence gaps and capture restarts.

## 4. For the AI teammate: receiving samples on Ultra96

**Integration point:** `process_physical_data()` in `ultra96/server.py`, called from each node's connection worker after frame assembly. It decodes:

| Data | Meaning |
|---|---|
| `node_id`, `session_id` | Sensor identity and current connection session |
| `sequence` | Capture sequence assigned once at acquisition; it does not reset at trigger/flush |
| `timestamp_us` | ESP32 monotonic capture time, in microseconds |
| `coordinator_timestamp_us` | Capture time mapped into Ultra96's monotonic clock; unavailable if no mapping exists |
| `ax, ay, az` | Three float32 acceleration values in m/s² |
| `gx, gy, gz` | Three float32 angular-rate values in rad/s |
| `status`, `flags` | Sensor validity, historical/mock/overflow and transport conditions |

The physical wire format is a 24-byte common header plus a 26-byte
`PHYSICAL_DATA` payload, all in network byte order. The payload is
`!ffffffH`; capture timestamp and sequence remain in the header and there is no
battery field. The version-1 raw `DATA` (`!hhhhhhHH`), TEST and benchmark
formats remain available for their existing demo paths. AI code should consume
decoded samples rather than implement another network parser.

Both physical and legacy decode paths return the neutral decoded-sample type.
The shared in-process adapter combines Node 1 and Node 2 into configurable,
overlapping capture-time windows and reports sample counts, sequence gaps, and
provisional cross-node timestamp skew. Benchmark and unmapped DATA are not
included. The current adapter logs and releases each completed window; it has
no AI-specific tensor conversion or inference dependency.

**Still to implement together:** a bounded queue/callback or local IPC bridge
from completed windows into the inference pipeline. Keep inference off the
receiving workers so slow inference cannot stall sensor ingestion.

For windows combining both sensors, use the mapped capture timestamps rather than packet arrival order. Agree on node/body placement, axis orientation, physical units/scaling, window size/overlap and missing-sample handling when replacing mock data. Physical clock alignment and sync thresholds still need validation with the real pipeline. Exclude benchmark/mock data as appropriate; status bits identify these conditions.

## 5. For the AI teammate: publishing results

The output interface already exists. From a process **on Ultra96**, connect by TCP to `127.0.0.1:9100` and send UTF-8 JSON, **one object followed by a newline** per update:

```json
{"v":1,"type":"exercise_update","run_id":12345,"seq":0,"ts_us":1000000,"exercise":"squat"}
```

| Field | Contract |
|---|---|
| `v`, `type` | Integer `1` and string `"exercise_update"` |
| `run_id` | Non-negative integer identifying the publisher run; change it when restarting the sequence |
| `seq` | Non-negative integer increasing for every update within that run |
| `ts_us` | Non-negative Ultra96 monotonic timestamp in microseconds, not wall-clock/Unix time |
| `exercise` | Non-empty string; current demo labels are `unknown`, `squat`, `push_up`, `sit_up`, `lunge`, `leg_raise` |

Use `visualizer_protocol.build_message()` and `serialize_message()` to construct messages, then append `\n` when writing to the publisher socket. Keep one owner of output ordering/run identity. The validator accepts other non-empty exercise names, but coordinate the vocabulary with Unity first.

Run the gateway without `--demo-source` for real inference. The existing sensor-server demo publisher also needs disabling/replacing: it currently publishes a **random exercise after a validated, non-replayed TEST_RESPONSE**, not after ordinary DATA or real inference. Avoid running competing demo and real publishers.

## 6. For the Unity teammate: consuming results

- Connect to **`wss://192.168.0.2:9000`** on the private Wi-Fi. This is the laptop's address; the WSS endpoint terminates on Ultra96 through the relay/tunnel.
- Each WebSocket text message contains one JSON object with the six fields above. Newlines are required only on the local AI-to-gateway TCP input.
- Reuse `Ultra96VisualizerClient`: subscribe to `UpdateReceived` for validated exercise updates and `StateChanged` for connection status. Both callbacks are delivered on Unity's main thread, so they can drive UI/animation safely. `Latest` holds the last accepted result.
- Updates with `seq <= Latest.Sequence` are ignored within the same `run_id`. A changed run ID allows the sequence to restart. Visualizer sequence numbers are separate from ESP32 sample sequences.
- No newer accepted update for 500 ms changes `LIVE` to `STALE`. Transport failures produce `DISCONNECTED`; reconnect delays are 1, 2, 4, then 5 seconds. An event-only AI publisher will therefore appear stale between results: agree on periodic state publication or adjust the freshness policy together.
- Bundle the gateway's public certificate at `Assets/Resources/Certificates/server-cert.bytes`. The client checks its exact SHA-256 pin and validity dates. Replacing the server certificate requires updating the bundled certificate. The private key stays on Ultra96.

The gateway permits **one connected visualizer** and uses a latest-only pending slot. Slow/disconnected phones can miss intermediate updates; on reconnect the gateway supplies its retained latest state. This suits displaying current exercise, but is not reliable delivery of every repetition/event. Rep count, confidence, feedback and acknowledgements are not part of the current six-field contract; agree on extensions before relying on them.

## 7. What we need to integrate next

1. Run the physical profiles with both MPU6050s and validate units, generic peak sensitivity, clock alignment and sustained 50 Hz timing.
2. Connect the neutral completed-window adapter to AI and agree on tensor shape, scaling, missing-window policy, and backpressure.
3. Publish real inference results through port 9100, with demo publishers disabled and a freshness cadence agreed with Unity.
4. Connect `UpdateReceived` to the final Unity visuals and test both sensor reconnects and phone reconnects end to end.

For launch commands and certificate setup, see [Ultra96 services](../ultra96/README.md), [laptop relays](../laptop/README.md), [Unity demo](../unity/Ultra96VisualizerDemo/README.md) and [two-node firmware](../firmware/FireBeetle_to_Ultra96/DEMO_TWO_NODES.md).
