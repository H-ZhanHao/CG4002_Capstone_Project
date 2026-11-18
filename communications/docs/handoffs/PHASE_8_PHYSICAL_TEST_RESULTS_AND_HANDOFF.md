# ESP32 Individual Demo: Phase 8 Physical Test Results and Handoff

> **Historical test record - 28 September 2026, Phase 8.**
> Physical Hub 3 and Hub 6 produced approximately 50 Hz synthetic IMU streams
> through the TLS laptop controller and SSH tunnel. The rates, latency examples,
> credentials limitation and benchmark totals below describe that tested setup,
> not the final physical-IMU build. Commands and the Phase 9 starting instructions
> require the matching historical source. See
> [current setup references](../README.md#component-guides),
> [recorded results and limits](../EXPERIMENTS_AND_RESULTS.md#4-concurrent-producers-and-recovery)
> and the retained [Phase 9 outcome](PHASE_9_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md).

This work prepared for the Individual Subsystem Test on 1 October; it does
not describe final-product integration.

Date recorded: 28 September 2026  
Repository: `D:\NUS\CG4002-communications`  
Physical devices: FireBeetle Hub 3 and FireBeetle Hub 6

## Purpose

This document records the implementation and physical verification of Phase 8
of the [individual-demo milestone sequence](../DESIGN_EVOLUTION.md#1-milestones-and-original-assessment-scope),
now preserved in the consolidated design record.
Read
`PHASE_1_TO_6_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md` and
`PHASE_7_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md` first for the earlier transport,
protocol, file-transfer, and physical-test state.

Phase 8 demonstrates that two physical hubs can maintain independent TLS and
application Sessions through one laptop controller and one Ultra96 server.
Requests, replay protection, stream statistics, benchmarks, file operations,
and reconnect behavior remain isolated by hub.

## Tested topology

```text
FireBeetle Hub 3 -- certificate-verified TLS --\
                                                  laptop controller
FireBeetle Hub 6 -- certificate-verified TLS --/          |
                                                           | SSH tunnel
                                                           v
                                                        Ultra96
```

Both hubs used the TLS firmware profiles now named `hub3_private_tls` and
`hub6_private_tls` (named `hub3_demo_tls` and `hub6_demo_tls` during the test).

## Implemented laptop behavior

`laptop/interactive_hub_relay.py` now maintains a separate `HubConnection` for
each connected hub. Each connection owns independent:

- TLS writer and write lock.
- Application Session ID and request Sequence.
- Pending request table and duplicate/conflict cache.
- Normal-stream and benchmark statistics.
- File-transfer targeting and result handling.

The interactive controls are:

```text
Hub target:  [3] select Hub 3   [6] select Hub 6   [a] choose a random hub
Request:     [any other key] send test request   [d] duplicate   [c] conflict
File test:   [f] valid transfer   [r] bad CRC   [o] bad offset   [t] timeout
Benchmark:   [b] start selected   [x] stop selected
             [m] start all        [n] stop all
Controller:  [q] quit
```

Hub 3 lines are cyan, Hub 6 lines are magenta, and global or combined lines
remain neutral. The complete hub-specific line is colored rather than only the
numeric Node ID. Log timestamps omit milliseconds for readability, while
explicit latency and rate measurements retain sub-second precision.

Windows TLS shutdown cleanup now cancels and collects every child relay task.
Expected close-time TLS errors are quiet during intentional shutdown, while
unexpected runtime disconnects remain visible.

## Physical test results

### Concurrent normal streams

Both hubs were connected concurrently with different application Sessions:

```text
Hub 6 session=2663537173
Hub 3 session=2668457290
ready_hubs=[3, 6]
```

Representative settled measurements were:

```text
Hub 6: 16.41 kbps / 51.3 packets/s
Hub 3: 16.29 kbps / 50.9 packets/s
Combined: 32.70 kbps / 102.2 packets/s
```

All observed normal-stream health counters remained zero:

```text
gaps=0
duplicates=0
out_of_order=0
timestamp_regressions=0
```

The initially higher rate in each short startup window settled to the intended
approximately 50 Hz per hub.

Result: **PASS**

### Targeted interactive requests

Hub 6 request and response:

```text
session=1519504486 sequence=1
request  accel=(-1200, 800, 16000) gyro=(-250, 500, -750)
response accel=(-1194, 800, 16000) gyro=(-150, 500, -750)
latency=138.2 ms
status 0x8000 -> 0xA000
```

Hub 3 request and response:

```text
session=1208747477 sequence=2
request  accel=(-1, 0, 1) gyro=(-100, 100, 200)
response accel=(2, 0, 1) gyro=(0, 100, 200)
latency=87.4 ms
status 0x8000 -> 0xA000
```

The diagnostic transformation is intentionally hub-identifying:

```text
response ax = saturating(request ax + hub Node ID)
response gx = saturating(request gx + 100)
```

The remaining readings are unchanged and status bit `0x2000` marks a processed
demo response. Ultra96 independently logged the matching Hub 6 response.

Result: **PASS**

### Independent duplicate and conflicting-replay state

For Hub 3, Sequence 3 was sent normally, repeated exactly with `d`, and then
reused with a different payload using `c`. The exact duplicate returned the
same cached response. The conflicting reuse returned:

```text
Hub ERROR hub=3 code=8 offending_type=0x20 sequence=3
```

The same sequence was independently exercised on Hub 6 using Sequence 2:

```text
Hub ERROR hub=6 code=8 offending_type=0x20 sequence=2
```

Ultra96 observed two identical Hub 6 responses with the same timestamp for the
normal request and cached duplicate, followed by the matching Hub 6 error.
This confirms that duplicate caches and Sequence state are per-hub.

Result: **PASS**

### Random targeting

Automatic random hub selection was exercised and requests continued to be
correlated with the Session, Sequence, and response of the selected physical
hub.

Result: **PASS**

### Individual Hub 3 benchmark

```text
elapsed=10000 ms
ESP frames=35024 bytes=1400960
laptop frames=35024 bytes=1400960
average=1113.61 kbps
queue_drops=0 send_failures=0
gaps=0 duplicates=0 out_of_order=0
esp_frame_difference=0
```

Hub 6 continued its normal approximately 50 Hz stream while Hub 3 was under
benchmark load.

Result: **PASS**

### Individual Hub 6 benchmark

```text
elapsed=10003 ms
ESP frames=41744 bytes=1669760
laptop frames=41744 bytes=1669760
average=1331.76 kbps
queue_drops=0 send_failures=0
gaps=0 duplicates=0 out_of_order=0
esp_frame_difference=0
```

Hub 3 continued its normal approximately 50 Hz stream while Hub 6 was under
benchmark load.

Result: **PASS**

### Concurrent benchmark

Both benchmarks were started together with control Sequence 5.

Hub 6 result:

```text
elapsed=10003 ms
ESP/laptop frames=39888
ESP/laptop bytes=1595520
average=1275.04 kbps
```

Hub 3 result:

```text
elapsed=10007 ms
ESP/laptop frames=33536
ESP/laptop bytes=1341440
average=1068.39 kbps
```

Both hubs reported:

```text
queue_drops=0
send_failures=0
gaps=0
duplicates=0
out_of_order=0
esp_frame_difference=0
```

Benchmark frames carry the benchmark status flag and are counted under
`BENCHMARK_LIVE` rather than normal `STREAM`; consequently a benchmarked hub
can temporarily show `STREAM ... 0.0pps` without indicating packet loss.

Result: **PASS**

### Physical reset and failure isolation

Hub 3 was reset while Hub 6 remained connected. The laptop observed:

```text
Hub 3 disconnected; ready_hubs=[6]
Hub 6 current=16.42 kbps / 51.3 packets/s
Hub 3 ready; session=2524549623; ready_hubs=[3, 6]
```

Hub 6 was then reset while Hub 3 remained connected:

```text
Hub 6 disconnected; ready_hubs=[3]
Hub 6 ready; session=3822639005; ready_hubs=[3, 6]
Hub 3 current=16.01 kbps / 50.0 packets/s
```

In both directions, the unaffected hub continued streaming and the reset hub
completed reconnection with a new application Session.

Result: **PASS**

### Phase 7 file-transfer compatibility

File transfer was retested through the multi-hub controller and continued to
work with the selected hub while the other connection remained available.

Result: **PASS**

### Clean two-hub shutdown

After the final Windows asyncio/TLS cleanup correction, intentional shutdown
with `q` completed without an unhandled-task traceback. Both hubs were removed
from the active set.

Result: **PASS**

## Automated verification

The latest laptop verification after the Phase 8 UI and shutdown refinements:

```text
Python compilation: PASS
laptop unittest discovery: 14 passed
git diff --check for laptop files: PASS
```

The Phase 7 handoff contains the most recent firmware-profile and Ultra96 test
results. Phase 8 did not require an ESP32 protocol change or an Ultra96 protocol
change.

## Known limitation: demo credentials are not unique

The implementation plan's Phase 8 acceptance gate calls for a unique credential
per hub. The current `HUB_CONFIG` deliberately retains the same predictable
32-byte development token for Hub 3 and Hub 6:

```python
DEVELOPMENT_DEVICE_TOKEN = bytes(range(32))
```

TLS encryption and server-certificate verification are active, and the SSH
tunnel protects the laptop-to-Ultra96 network segment. However, this token is
not a production authentication secret and does not provide unique cryptographic
hub identity. It currently demonstrates the application authentication field
and comparison path only.

For production, provision a different random 32-byte secret for each hub,
store ESP values in ignored private configuration, and load Ultra96 values from
protected environment or secret storage. No protocol-format redesign is needed.

The physical evidence supplied for this handoff also records new Session IDs
after reset but does not include the corresponding Ultra96 `HELLO accepted`
Boot ID lines. Capture those lines during Phase 9 reliability testing to prove
new Boot ID allocation explicitly.

## Phase 8 acceptance summary

```text
Two concurrent physical TLS connections: PASS
Independent application Sessions: PASS
Approximately 50 Hz per hub for normal streaming: PASS
Approximately 32 kbps combined application throughput: PASS
Per-hub Node ID, Sequence state, request cache and statistics: PASS
Alternating and random requests: PASS
Per-hub duplicate/conflict protection: PASS
Individual benchmarks: PASS
Concurrent benchmark: PASS
Zero benchmark drops and ESP/laptop count difference: PASS
Failure isolation in both reset directions: PASS
Per-hub colors and readable statistics: PASS
Phase 7 file-transfer compatibility: PASS
Clean two-hub shutdown: PASS
Unique production-grade per-hub credentials: NOT IMPLEMENTED (documented demo limitation)
Explicit new Boot ID evidence after reset: TO CAPTURE IN PHASE 9
```

Phase 8 concurrent communications behavior is implemented and physically
verified on Hubs 3 and 6. Credential uniqueness remains a documented security
hardening item rather than a completed Phase 8 property.

## Phase 9 starting point

Phase 9 focuses on deliberate reliability failures and recovery evidence:

1. Record both hubs' initial Ultra96 `HELLO accepted` Boot IDs and Session IDs.
2. Power-reset Hub 3 while Hub 6 continues, then confirm both a new Boot ID and
   new Session for Hub 3.
3. Move Hub 6 out of range or otherwise remove only its network connectivity
   while Hub 3 continues.
4. Restore Hub 6 and observe bounded reconnect backoff, TLS verification,
   `HELLO`, and a new Session.
5. Interrupt a benchmark, reconnect, and prove benchmark state does not resume
   automatically under the new Session.
6. Interrupt a file transfer, reconnect, and prove a fresh `FILE_BEGIN` is
   required rather than resuming stale state.
7. Confirm stale commands from an earlier Session are rejected or never
   executed after reconnect.

The unaffected hub's stream and statistics must remain live throughout each
single-hub failure.
