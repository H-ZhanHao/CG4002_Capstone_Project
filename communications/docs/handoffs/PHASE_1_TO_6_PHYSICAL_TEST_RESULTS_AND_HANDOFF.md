# ESP32 Individual Demo: Phase 1–6 Physical Test Results and Handoff

> **Historical test record - 27 September 2026, Phases 1-6.**
> Physical Hub 3 transmitted approximately 50 Hz synthetic IMU data through the
> certificate-verified TLS laptop relay and SSH route. The throughput, heap,
> frame sizes, failures and test counts below describe that revision, not final
> 100 Hz physical-sensor performance. Build/terminal commands and the Phase 7
> starting instructions are historical; use matching saved source to reproduce
> them. See [current setup references](../README.md#component-guides),
> [recorded results and limits](../EXPERIMENTS_AND_RESULTS.md#3-single-producer-transport-and-file-integrity)
> and the retained [Phase 7 outcome](PHASE_7_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md).

This work prepared for the Individual Subsystem Test on 1 October; it does
not describe final-product integration.

Date recorded: 27 September 2026  
Repository: `D:\NUS\CG4002-communications`

## Purpose

This document records what was implemented and physically observed for Phases
1–6 of the [individual-demo milestone sequence](../DESIGN_EVOLUTION.md#1-milestones-and-original-assessment-scope),
now preserved in the consolidated design record.
It is also intended as the starting context for a new chat continuing with
Phase 7.

## Current tested path

```text
FireBeetle Hub 3
  -> verified TLS connection to laptop demo controller
  -> laptop protocol-aware relay and measurement
  -> SSH tunnel toward Ultra96
  -> Ultra96 protocol server
```

The FireBeetle-to-laptop hop is protected by TLS with CA and expected-server-identity verification. The laptop-to-Ultra96 hop is carried through the SSH tunnel.

## Phase 1: Protocol foundation

### Implemented

- Extended the fixed-header binary protocol with codecs and validation for:
  - `TEST_REQUEST` / `TEST_RESPONSE`
  - `BENCHMARK_CONTROL` / `BENCHMARK_REPORT`
  - File-transfer messages required by a later phase
  - Protocol `ERROR` messages
- Kept serialization independent from TCP, TLS, Wi-Fi, and application behavior.
- Added host-side protocol tests.

### Verified

The host protocol test printed:

```text
protocol host tests: PASS
```

The following firmware environments built successfully:

```text
hub3
hub3_private_relay
hub6_private_relay
leaf1
```

## Phase 2: Application/transport separation

### Implemented

- The transport task is the sole owner of the socket/TLS connection.
- Complete received frames enter a bounded application-command queue.
- Logical application responses return through a separate bounded response queue.
- Session start/end clears stale queued commands and responses.
- Outbound sensor buffering remains bounded and drops the oldest sample when full.

### Physical observation

Hub 3 connected, completed HELLO, obtained a nonzero session, and continued sending DATA:

```text
TLS/TCP connection established
Sending HELLO: hub=3 ... assigned_mask=0x0E
HELLO accepted; session=<nonzero>
Application session started: <same session>
Sent: node=3 sequence=... queue=0
```

An initial failed smoke test was caused by using the wrong ESP destination port (`15000` expected by the demo controller). After correcting the process/port setup, the smoke test passed.

## Phase 3: Interactive request processing

### Implemented

- Laptop keyboard generates canonical `TEST_REQUEST` messages.
- Hub 3 deliberately transforms the request:
  - `ax = ax + hub_node_id`, with signed 16-bit saturation.
  - `gx = gx + 100`, with signed 16-bit saturation.
  - `PROTOCOL_STATUS_TEST_REQUEST_PROCESSED` is added.
- Exact repeated sequence/payload returns a cached response.
- Reusing a sequence with different payload returns error code 8.
- Ultra96 accepts and displays hub-originated protocol errors.

### Physical observations

Normal processing was verified. Example:

```text
request:  ax=-1200 gx=-250
response: ax=-1197 gx=-150
```

This matches Hub 3's transformation: `-1200 + 3 = -1197` and `-250 + 100 = -150`.

Duplicate behavior was verified:

- Pressing `d` returned the cached response.
- The duplicate response retained the same response timestamp and payload.

Conflict behavior was verified:

```text
Hub ERROR code=8 offending_type=0x20 sequence=<reused sequence>
HUB_ERROR ... code=8 offending_type=0x20
```

An earlier version caused reconnection because Ultra96 rejected the hub-originated `ERROR`. Ultra96 was corrected to parse it, after which the conflict was reported without an unwanted reconnect.

## Phase 4: TLS transport

### Implemented

- Added TLS transport support using ESP-TLS while keeping application framing unchanged.
- The ESP verifies both:
  - The configured CA certificate.
  - The expected server identity `demo-relay.local`.
- Added TLS firmware profiles:
  - `hub3_private_tls` (then named `hub3_demo_tls`)
  - `hub6_private_tls` (then named `hub6_demo_tls`)
- Laptop controller can serve TLS using a certificate and private key.
- The private certificate header is ignored by Git.

### Physical observations

Successful positive test:

```text
TLS connection established
Sending HELLO: hub=3 ...
HELLO accepted; session=<nonzero>
```

`TEST_REQUEST`, duplicate handling, conflict handling, normal DATA, and Ultra96 forwarding all worked over TLS.

Negative identity-verification test:

```c
#define DEMO_TLS_SERVER_IDENTITY "wrong-demo-relay.local"
```

The ESP correctly failed closed:

```text
mbedtls_ssl_handshake returned -0x2700
TLS connection or certificate verification failed
```

No HELLO or application DATA was accepted under the wrong identity. The correct identity was restored afterward:

```c
#define DEMO_TLS_SERVER_IDENTITY "demo-relay.local"
```

The PEM embedding initially failed to compile because certificate lines were not valid quoted C strings. The private header was corrected and both TLS profiles subsequently built.

## Phase 5: Normal one-minute stream

### Implemented

- Preserved the deterministic 50 Hz mock producer.
- Laptop reports aggregate statistics every five seconds:
  - Current and average kbps
  - Packets per second
  - Total frames and bytes
  - Sequence gaps, duplicates, out-of-order frames
  - Timestamp regressions
  - Latest decoded IMU sample
- ESP's once-per-second normal status includes:
  - Queue depth
  - Current free heap
  - Minimum free heap observed since boot

### Physical observations

The stream was left running for approximately 15 minutes. Representative laptop output after about 801 seconds:

```text
STREAM hub=3 stream_age=801.2s current=15.66kbps/48.9pps
average=16.02kbps frames=40107 bytes=1604280
gaps=0 duplicates=0 out_of_order=0 timestamp_regressions=0
```

Representative ESP output:

```text
queue=0 heap=174668 min_heap=165548
```

Observed conclusions:

- Long-term rate remained approximately 50 packets/s and 16 kbps.
- All sequence/timestamp integrity counters remained zero.
- The outbound queue remained at zero during stable operation.
- Current heap returned to a stable level rather than continually decreasing.
- Minimum heap remained fixed, providing no indication of a memory leak.

Phase 5 acceptance criteria were satisfied.

## Phase 6: Maximum-throughput benchmark

### Implemented

- Laptop controls:
  - `b`: start a ten-second benchmark.
  - `x`: stop the active benchmark early.
- Normal 50 Hz production pauses during the benchmark and resumes afterward.
- Benchmark DATA uses a dedicated status bit so normal and benchmark sequence/statistics state remain separate.
- Frames are encoded into bounded batches of 16 complete 40-byte DATA frames.
- The laptop independently measures received benchmark frames and throughput.
- ESP returns `BENCHMARK_REPORT` with elapsed time, frames, bytes, queue drops, and send failures.
- Ultra96 parses benchmark DATA and the final report while limiting console output.
- Normal heartbeats are suppressed during benchmark mode because continuous DATA already proves liveness and heartbeat traffic would contaminate the measurement.
- Every START resets the benchmark's sequence and counters.
- The final laptop comparison reports the difference between ESP-sent and laptop-received frame totals.

### Defects found through physical testing

1. A 640-byte batch array was originally placed on the TLS-heavy `tcp_client` task stack and caused:

   ```text
   ***ERROR*** A stack overflow in task tcp_client has been detected.
   ```

   It was moved into a fixed 640-byte static RAM buffer owned only by the singleton transport task. This is bounded, avoids heap fragmentation, and removed the stack overflow.

2. Benchmark totals originally accumulated across repeated runs. START now resets frames, bytes, failures, and benchmark sequence for every independent run.

3. Heartbeats originally continued once per second during the benchmark. They are now disabled only while benchmark mode is active and resume in normal mode.

### Final physical observations

Full ten-second run:

```text
BENCHMARK_REPORT control_sequence=1 elapsed=10002ms
frames=31920 bytes=1276800 queue_drops=0 send_failures=0

BENCHMARK_RECEIVED frames=31920 bytes=1276800
average=1020.12kbps gaps=0 duplicates=0 out_of_order=0
esp_frame_difference=0
```

Manual early-stop run:

```text
BENCHMARK_REPORT control_sequence=2 elapsed=2100ms
frames=2720 bytes=108800 queue_drops=0 send_failures=0

BENCHMARK_RECEIVED frames=2720 bytes=108800
average=409.69kbps gaps=0 duplicates=0 out_of_order=0
esp_frame_difference=0
```

Ultra96 also displayed the matching benchmark report. The full run achieved approximately 1.02 Mbps and 3,190 DATA frames/s at the application-frame level. This excludes TLS, TCP/IP, and Wi-Fi overhead.

Observed conclusions:

- ESP-sent and laptop-received totals matched exactly.
- No sequence gaps, duplicates, out-of-order frames, queue drops, or send failures occurred.
- Automatic ten-second STOP worked.
- Manual STOP was processed successfully.
- Normal 50 Hz production resumed after each run.
- The tested maximum application rate was roughly 64 times the normal single-hub requirement.

Phase 6 acceptance criteria were satisfied for Hub 3.

## Current keyboard controls

In `laptop/interactive_hub_relay.py`:

```text
normal key  send a random TEST_REQUEST
d           send an exact duplicate
c           reuse the last sequence with conflicting payload
b           start a ten-second maximum-throughput benchmark
x           stop the benchmark early
q           terminate the controller
Ctrl+C      terminate the controller
```

Keyboard handling starts before a hub connects, so `q` and Ctrl+C also work while the controller is only listening.

## Important files through Phase 6

### ESP32 firmware

- `firmware/FireBeetle_to_Ultra96/include/protocol.h`
- `firmware/FireBeetle_to_Ultra96/src/protocol.c`
- `firmware/FireBeetle_to_Ultra96/include/application.h`
- `firmware/FireBeetle_to_Ultra96/src/application.c`
- `firmware/FireBeetle_to_Ultra96/src/tcp_client.c`
- `firmware/FireBeetle_to_Ultra96/src/mock_imu.c`
- `firmware/FireBeetle_to_Ultra96/include/demo_tls_config.h` (private and ignored)
- `firmware/FireBeetle_to_Ultra96/include/demo_tls_config.example.h`
- `firmware/FireBeetle_to_Ultra96/platformio.ini`

### Laptop

- `laptop/interactive_hub_relay.py`
- `laptop/test_interactive_hub_relay.py`
- `laptop/tcp_relay.py` remains the separate transparent relay.

### Ultra96

- `ultra96/server.py`

## Current verification status

Most recent automated checks:

```text
laptop controller tests: 5 passed
Ultra96 server tests: 4 passed
hub3_private_tls (then named hub3_demo_tls): SUCCESS
hub6_private_tls (then named hub6_demo_tls): SUCCESS
```

The broader Ultra96 test discovery also includes a visualizer-gateway test requiring the optional Python `websockets` package. Its absence in the local sandbox is unrelated to the protocol/server tests above.

PlatformIO currently prints a board configuration warning that the profile expects 16 MB flash while the connected chip reports 2 MB. The application still fits the current 1 MB factory partition (about 878 KB), but the board/flash setting should be reconciled before relying on additional flash capacity.

## Phase 7 starting point: File transfer

The next phase from the implementation plan requires:

1. A bounded, session-scoped file-transfer state machine on the ESP.
2. Validation of metadata, offsets, sizes, and per-chunk CRC32.
3. Streaming SHA-256 without storing the entire file in RAM.
4. Forwarding accepted file messages through the existing transport toward Ultra96.
5. A final `FILE_RESULT` and complete state cleanup.
6. Abort behavior on timeout, disconnect, malformed chunks, or session change.

Acceptance targets include a 64–256 KiB file with identical SHA-256 at laptop, ESP, and Ultra96; deliberate CRC and offset failures; disconnect recovery requiring a fresh `FILE_BEGIN`; and bounded heap use.

Important design constraints to preserve:

- `tcp_client` remains the sole owner of the TLS connection.
- Application commands and responses cross bounded queues.
- No entire file should be buffered on the ESP.
- File-transfer state must be cleared on session end/reconnect.
- Keep normal streaming and benchmark behavior unchanged outside an active file transfer.
- Avoid large local arrays on the `tcp_client` task stack; the Phase 6 physical test demonstrated why bounded static storage or carefully sized application-task storage is safer.

