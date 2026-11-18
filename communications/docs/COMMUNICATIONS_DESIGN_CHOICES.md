# Communications design choices

Reviewed against the repository on 16 November 2026.

This document explains selected communications decisions in the FitSense wearable exercise system. It uses the independent Node 1/Hip and Node 2/Wrist firmware, the integrated `server/server_rep_fall.py` entry point, and the Unity visualizer path. Current physical capture is 100 Hz; the integrated server defaults to three-second windows with a one-second hop. Earlier phase documents describe different rates, window lengths, and hub topologies.

The network experiments provide direct evidence for the selected route. The other explanations describe the engineering benefits and tradeoffs of the checked-in implementation; they do not imply that every alternative was experimentally compared.

Final firmware environments are `node1_final` and `node2_final`, with four
separate sampling diagnostic environments. For operational commands use
[Network setup](NETWORK_SETUP.md), the
[firmware guide](../firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md) and
[server selection/policies](../ultra96/README.md#choose-one-sensor-server).
[Design evolution](DESIGN_EVOLUTION.md) records earlier alternatives;
[Experiments and results](EXPERIMENTS_AND_RESULTS.md) identifies the measured
evidence and remaining validation. This review did not operate deployed devices.

## 1. A private Wi-Fi LAN and laptop bridge provide a usable route

The wearables join the private DIR-842 LAN. The laptop connects to that LAN and uses its campus/VPN connection to reach Ultra96 through SSH forwarding.

```text
Node 1 / Hip   --\
                 private Wi-Fi -> laptop relay -> SSH tunnel -> Ultra96
Node 2 / Wrist --/

Android phone <-> private Wi-Fi <-> laptop opaque relay
                                      <-> SSH tunnel <-> Ultra96 WSS gateway
```

**Why:** In the recorded campus tests, the ESP32 could associate with NUS Wi-Fi but could not establish the required TCP connections to either the laptop listener or Ultra96 port 5000. The working route therefore uses a network segment we control for local device traffic and the laptop's established SSH route for remote access. The private adapter has no default gateway or DNS, allowing the laptop's campus connection to remain its Internet/VPN route.

The embedded-SSH experiment also found that the usable route required reaching Ultra96 SSH through the jump host; a simple jump-host forward to the application port was rejected. Keeping this routing work on the laptop avoids adding nested SSH to the ESP32 firmware.

**Tradeoff:** The router, laptop, relay, VPN, and SSH tunnel become deployment dependencies. The laptop and tunnel are shared failure points. The campus results document one tested environment and do not establish the exact firewall or isolation mechanism responsible.

Evidence: [NUS network test results](results/NUS_NETWORK_TEST_RESULTS.md).

## 2. Each wearable is an independent producer

Each ESP32 has its own identity, TLS/TCP connection, transport session, and clock mapping. The communications firmware has no ESP-NOW forwarding implementation or leaf-to-hub relationship; both boards connect directly to the laptop relay. Throughput benchmarks generate DATA frames locally and send them over the existing TLS connection.

**Why:** Direct producers keep sensor ownership clear and remove an intermediate wearable forwarding hop. Separate connections also let Ultra96 track synchronization, acknowledgements, and failures per node. The integrated dynamic coordinator lets a synchronized node start independently and lets a second node join the existing timeline with a live-only start.

**Tradeoff:** Ultra96 must coordinate concurrent connections and changing source availability. The AI teammate's updated implementation supports single-node inference. The [server guide](../ultra96/README.md#choose-one-sensor-server) records the local-copy version difference. 

Evidence: [Hip and Wrist firmware guide](../firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md) and [dynamic coordinator](../../server/dynamic_capture.py).

## 3. TCP supplies ordered delivery; the application supplies framing

Sensor and control messages share a bidirectional TCP connection, protected by TLS on the wearable-facing link.

**Why:** Ordered, reliable delivery fits the history flush: earlier captured samples should precede later samples. The connection also carries handshakes, clock exchanges, capture commands, and acknowledgements, reducing the number of transports the firmware must manage.

TCP exposes a byte stream. The receiver accumulates bytes and uses the protocol's fixed header and payload length to reconstruct frames, handling both partial frames and multiple frames in one read.

**Tradeoff:** Retransmission can delay newer data behind missing bytes. Reliable delivery alone does not provide a latency deadline, retain unlimited history, or prove that a device executed a command. We still need bounded buffers, timeouts, and application acknowledgements.

Evidence: [firmware transport](../firmware/FireBeetle_to_Ultra96/src/tcp_client.c) and [integrated server](../../server/server_rep_fall.py).

## 4. Binary IMU frames and JSON phone messages suit different workloads

The wearable protocol has a 24-byte header with version, message type, node identity, payload length, session, sequence, and timestamp fields. Multi-byte values use network byte order. A physical sample adds six `float32` values in SI units and a two-byte status field, producing a 50-byte frame.

**Why:** A defined binary layout gives the C firmware and Python server a predictable encoding and buffer bound. Physical units make the sensor interface explicit for downstream processing. Version and message-type fields allow receivers to distinguish formats and reject unsupported messages.

At 100 Hz, the application traffic is:

| Producers streaming | Sample frames per second | Application bytes per second |
| --- | ---: | ---: |
| One node | 100 | 5,000 |
| Two nodes | 200 | 10,000 |

The two-node figure is 80 kbit/s before control traffic, history bursts, TLS, TCP/IP, SSH, and Wi-Fi overhead. It is a size calculation, not measured link throughput.

The phone receives lower-rate exercise state and exchanges rep/fall messages as versioned JSON over WSS. Named fields make this interface easier to inspect and integrate with Unity. Internal Ultra96 services use newline-delimited JSON to mark message boundaries.

**Tradeoff:** Binary messages require matching serializers and are less readable in raw captures. JSON is more verbose, and supporting two formats requires validation at their boundary. The older integer `DATA` frame is 38 bytes; it must not be confused with the current 50-byte `PHYSICAL_DATA` frame.

Evidence: [protocol definitions](../firmware/FireBeetle_to_Ultra96/include/protocol.h), [binary encoding](../firmware/FireBeetle_to_Ultra96/src/protocol.c), and [visualizer protocol](../ultra96/visualizer_protocol.py).

## 5. Encryption follows the actual relay boundaries

The two routes have different TLS endpoints:

| Route | TLS endpoint | Onward protection |
| --- | --- | --- |
| Wearable to interactive laptop relay | Laptop | SSH tunnel carries the binary protocol to Ultra96 |
| Phone through transparent laptop relay | Ultra96 WSS gateway | TLS remains intact through the relay and tunnel |

**Why:** The interactive wearable relay parses messages and supports diagnostics, benchmarks, and file transfers, so it needs access to application bytes. The phone relay only forwards bytes; its TLS session can continue through to Ultra96. Unity authenticates the configured gateway using an exact certificate SHA-256 pin and certificate validity dates.

Internal publisher/control services remain on Ultra96 loopback. This supports local integration without exposing those plaintext service interfaces directly to the private LAN.

**Tradeoff:** The laptop is a trusted application endpoint for wearable data and holds the wearable-facing TLS key. Phone certificate replacement requires updating the trusted pin. The wearable route is protected in two encrypted legs, with plaintext available inside the laptop relay process; it is not one uninterrupted ESP32-to-Ultra96 TLS session.

Evidence: [interactive relay](../laptop/interactive_hub_relay.py), [WSS gateway](../ultra96/visualizer_gateway.py), and [Unity certificate validation](../unity/Ultra96VisualizerDemo/Assets/Scripts/Networking/Ultra96VisualizerClient.cs).

## 6. Capture timestamps and clock mapping align the two sensors

Each ESP32 timestamps acquisition using its local monotonic clock. Ultra96 estimates an independent mapping for each node using four timestamps:

```text
t1 = server sends SYNC_REQ       t2 = node receives it
t3 = node sends SYNC_RESP        t4 = server receives it

node-minus-server offset = ((t2 - t1) + (t3 - t4)) / 2
network round-trip delay = (t4 - t1) - (t3 - t2)
mapped capture time      = node capture time - offset
```

**Why:** Packet arrival time includes Wi-Fi, relay, tunnel, and queueing delays. Mapping capture timestamps lets the assembler place Hip and Wrist measurements on a common timeline despite those delivery delays.

The server targets five usable exchanges, selects the three lowest-delay rounds, checks their offset spread, and takes the median offset. Periodic refresh handles drift. Large corrections require confirmation and are approached gradually to limit abrupt timeline changes. A reconnect creates a fresh session that requires a new mapping.

**Tradeoff:** This is a software estimate affected by asymmetric network delay. It does not establish exact simultaneous sampling or a guaranteed timing error. Quality thresholds and actual alignment still need physical measurement under representative load.

Evidence: clock synchronization in [the integrated server](../../server/server_rep_fall.py).

## 7. Continuous local capture preserves movement before the trigger

Sampling continues while the node waits for activity. The firmware retains a bounded rolling history, currently seven seconds with capacity for 701 samples at 100 Hz. A generic movement peak can trigger `START_FLUSH`; the integrated server normally requests the preceding three seconds. The feeder sends retained samples oldest-first, catches up, and continues live.

**Why:** Trigger detection and network control take time. Keeping history preserves the beginning of the movement, while continuous capture keeps acquisition independent of whether streaming has started. The local peak is an activity cue; exercise classification belongs to the Ultra96 AI pipeline.

Sequences and timestamps are assigned before buffering. A separate transport task and nonblocking queue handoff reduce the effect of network stalls on acquisition. The history feeder preserves acquisition order across historical and live data.

**Tradeoff:** History consumes RAM and its flush creates a temporary traffic burst. Retention is finite: a sufficiently long stall can overwrite unsent samples. The firmware exposes loss/status information and rejects a history request it cannot satisfy, so the server can detect incomplete coverage. Triggered transmission can reduce idle sample traffic, but this does not establish a measured battery saving because capture and connectivity continue.

Evidence: [history configuration](../firmware/FireBeetle_to_Ultra96/include/sensor_history.h), [history feeder](../firmware/FireBeetle_to_Ultra96/src/sensor_history.c), and [physical pipeline](../firmware/FireBeetle_to_Ultra96/src/physical_pipeline.c).

## 8. Ultra96 owns overlapping windows and checks source quality

The integrated server defaults to a three-second window and a one-second hop. At 100 Hz, the nominal count is 300 samples per node per window. A late or starting partner gets a bounded capture-time grace period, configured as 250 ms by default.

**Why:** Building overlap centrally lets wearables send each acquired sample once. Ultra96 reuses retained samples across inference windows and applies consistent timing and quality rules. The one-second hop allows a new inference opportunity each second after sufficient data is available.

The current assembler retains real samples, checks coverage, and omits unusable nodes. It does not zero-pad missing measurements or trim real samples merely to force the nominal count. The integrated server configures a default missing-sample allowance of 4% per included node, or 12 samples for a nominal 300-sample window. Downstream preprocessing resamples the accepted real-sample count onto the model's fixed input grid.

**Tradeoff:** This policy balances availability against completeness; it is not a strict zero-loss requirement. Small accepted gaps can affect the time interpretation of arrays consumed by the model, and omitted nodes still require AI support. Window length, tolerances, and model input expectations must be kept consistent. A one-second hop is an inference schedule, not an end-to-end latency guarantee.

Evidence: [window assembler](../../server/window_assembler_updated.py) and [server defaults](../../server/server_rep_fall.py).

## 9. Sessions, identities, and ACKs make recovery explicit

`HELLO` validates node identity, capabilities, assigned source mask, and device token. The server allocates a nonzero transport session. Later validation uses sessions, sequences, capture timestamps, boot identities, and arm generations to distinguish current work from stale or repeated messages.

**Why:** TCP delivery does not prove that a command passed firmware state checks or was applied by the sampling task. `CONTROL_ACK` reports the command sequence, result, and effective local time. Samples enter capture only through the coordinator's acknowledged node state.

Rep feedback has bounded retries: 750 ms spacing, up to three send attempts, and a 2.5-second deadline. Retries preserve identity, and firmware duplicate suppression prevents an accepted command from replaying its physical output. The phone supplies rep events; the server validates them and routes the Hip beep.

**Tradeoff:** More identity and state tracking adds implementation complexity. An ACK establishes firmware acceptance, without physically measuring a buzzer or LED. Retry behavior is command-specific: the final fall-detection setting uses an explicit ON/OFF command with a two-second ACK deadline and no automatic replay on reconnect. An unknown result stays unknown until confirmed.

Evidence: [feedback sender](../../server/feedback_delivery.py) and [firmware duplicate handling](../firmware/FireBeetle_to_Ultra96/src/feedback_delivery.c).

## 10. A separate WSS gateway isolates phone delivery

Ultra96 publishes exercise state to a separate gateway process over loopback TCP. The gateway owns phone TLS/WebSocket connections, validation, reconnect handling, and rate limiting.

**Why:** A phone disconnect should not stop sensor ingestion or AI processing. Exercise classification represents current state, so the gateway keeps a single latest pending update and retains the newest value for a reconnecting phone. Replacing superseded state prevents a slow phone from building an increasingly stale classification backlog.

Rep results and fall notifications have separate handling and acknowledgement semantics because individual events cannot be treated as interchangeable state updates. Falls also use a local device event path that can operate while exercise streaming is stopped, when detection has been enabled.

**Tradeoff:** Intermediate classification states can be skipped, and the gateway currently serves one active visualizer. This path is designed for live feedback; a complete historical record would need separate storage. Gateway/process separation also adds a local service boundary to deploy and diagnose.

Evidence: [gateway implementation](../ultra96/visualizer_gateway.py), [rep control](../../server/rep_control.py), and [fall delivery](../../server/fall_delivery.py).
