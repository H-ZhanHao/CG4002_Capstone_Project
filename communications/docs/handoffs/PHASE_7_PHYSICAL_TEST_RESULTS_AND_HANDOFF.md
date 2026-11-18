# ESP32 Individual Demo: Phase 7 Physical Test Results and Handoff

> **Historical test record - 27 September 2026, Phase 7.**
> These physical Hub 3 file-transfer tests used the TLS laptop relay/SSH route
> with approximately 50 Hz synthetic IMU traffic continuing on the connection.
> Byte counts, hashes, negative tests and recovery results remain specific to
> that revision. File paths, commands and the Phase 8 starting instructions below
> are historical; they are not a current deployment procedure. See
> [current setup references](../README.md#component-guides),
> [recorded results and limits](../EXPERIMENTS_AND_RESULTS.md#3-single-producer-transport-and-file-integrity)
> and the retained [Phase 8 outcome](PHASE_8_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md).

This work prepared for the Individual Subsystem Test on 1 October; it does
not describe final-product integration.

Date recorded: 27 September 2026  
Repository: `D:\NUS\CG4002-communications`  
Physical device: FireBeetle Hub 3

## Purpose

This document records the implementation and physical verification of Phase 7
of the [individual-demo milestone sequence](../DESIGN_EVOLUTION.md#1-milestones-and-original-assessment-scope),
now preserved in the consolidated design record.
Read
`PHASE_1_TO_6_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md` first for the earlier
implementation state, TLS topology, and Phase 1–6 evidence.

Phase 7 demonstrates bounded, streaming file transfer through the existing
certificate-verified TLS and application protocol path. The ESP validates and
forwards one bounded chunk at a time; it never retains the complete file in
RAM.

## Tested path

```text
laptop file sender
  -> certificate-verified TLS
  -> FireBeetle Hub 3 validation and streaming SHA-256
  -> laptop protocol-aware relay
  -> SSH tunnel
  -> Ultra96 disk-backed reconstruction and SHA-256 verification
```

Normal 50 Hz `DATA` continued over the same connection during file-transfer
tests.

## Implemented behavior

### ESP32

- Session-scoped, single-active-transfer state machine.
- Conservative maximum accepted file size of 1 MiB.
- Default laptop chunk data size of 900 bytes; protocol maximum remains 1,008
  bytes.
- Validation of file ID, total size, chunk size, reserved fields, expected
  offset, chunk length, final size, and Session ID.
- CRC-32/ISO-HDLC compatible with Python `zlib.crc32` for each chunk.
- Streaming PSA SHA-256 without buffering the complete file.
- Accepted `FILE_BEGIN`, `FILE_CHUNK`, and `FILE_END` messages are forwarded
  through the existing bounded application/transport queues.
- Terminal `FILE_RESULT` is sent through the transport task to both the laptop
  and Ultra96.
- Ten-second inactivity timeout.
- Complete cleanup after success, validation failure, timeout, disconnect, or
  Session change.
- The transport task remains the sole TLS owner.
- Maximum-sized queue workspaces are statically allocated to avoid placing
  approximately 1 KiB file frames on the TLS-heavy transport stack.

### Laptop controller

`laptop/interactive_hub_relay.py` accepts `--file <path>` and provides:

```text
f  valid complete file transfer
r  deliberately corrupt the first chunk CRC
o  deliberately send the first chunk at the wrong offset
t  send FILE_BEGIN only to exercise the ESP inactivity timeout
```

The sender paces chunks so the existing fixed-size ESP queues remain bounded
while normal `DATA` continues. A physical connection loss during transmission
is logged as `FILE_SEND_ABORTED` and does not terminate keyboard handling.

### Ultra96

- Reconstructs an accepted transfer into a temporary disk file.
- Independently validates file metadata, ordering, CRC32, total length, and
  SHA-256.
- Compares a successful ESP `FILE_RESULT` against its independently calculated
  result.
- Deletes incomplete temporary files and clears state after a failed
  `FILE_RESULT` or disconnected client.

## Physical test results

### Valid 64 KiB transfer

Source size:

```text
65536 bytes
```

SHA-256 observed at laptop, ESP, and Ultra96:

```text
6276f7d9602ee9fbe7962449faae7e85fecdc8718e36be964f81360e80bc3c19
```

Observed result:

```text
FILE_RESULT result=0 bytes=65536
FILE_VERIFIED bytes=65536
```

The transfer used 73 chunks. Normal streaming continued at approximately 50
packets/s with zero gaps, duplicates, out-of-order frames, and timestamp
regressions.

Result: **PASS**

### CRC-corrupted chunk

The laptop deliberately changed the first chunk's transmitted CRC.

Observed result:

```text
FILE_RESULT result=3 bytes=0
File transfer rejected: result=3 offset=0
```

The corrupted chunk was not forwarded to Ultra96, and Ultra96 did not report
`FILE_VERIFIED`.

Result: **PASS**

### Wrong chunk offset

The laptop sent the first chunk at offset 1 instead of offset 0.

Observed result:

```text
FILE_RESULT result=2 bytes=0
File transfer rejected: result=2 offset=0
```

After the Ultra96 cleanup correction described below, this rejection did not
disconnect TLS or prevent a subsequent valid transfer.

Result: **PASS**

### Inactivity timeout

The laptop sent `FILE_BEGIN` and deliberately sent no chunks or `FILE_END`.
Normal sensor `DATA` continued while the file state was idle.

After approximately ten seconds, the ESP reported:

```text
File transfer timed out: id=3787876111 offset=0
FILE_RESULT result=6 bytes=0
```

A subsequent `f` transfer succeeded in the same TLS Session with the expected
SHA-256, proving that timeout cleanup removed all stale transfer state.

Result: **PASS**

### Mid-transfer disconnect and fresh-Session recovery

A 256 KiB file was used to provide time for a physical reset.

Source size and SHA-256:

```text
262144 bytes
978be40f8e9fd00e7e23e4bcf2befa5f11caa9b3e76b4d74ee64ca0f383e9ae6
```

The first attempt used:

```text
file_id=2444326675
session=631012834
```

Hub 3 was physically reset after approximately 160 of 292 chunks. The laptop
reported:

```text
FILE_SEND_ABORTED id=2444326675 connection lost
```

Ultra96 received `FILE_BEGIN` for the interrupted attempt but did not report
`FILE_VERIFIED`.

After reboot, Hub 3 completed TLS and HELLO again and received a new Session:

```text
session=3451194600
```

The new attempt used a fresh file ID and restarted from offset zero:

```text
file_id=3422588183
FILE_RESULT result=0 bytes=262144
FILE_VERIFIED bytes=262144
sha256=978be40f8e9fd00e7e23e4bcf2befa5f11caa9b3e76b4d74ee64ca0f383e9ae6
```

The interrupted transfer did not resume under the new Session.

Result: **PASS**

### Bounded queue, heap, and stream behavior

Representative successful-transfer ESP observations included:

```text
queue=0
heap approximately 160000 to 167000 bytes
minimum heap approximately 153992 bytes
```

The current heap returned to stable values rather than declining with the
number or size of chunks. Normal `DATA` continued during valid transfer and
timeout tests. During the deliberate physical reset, the bounded outbound
sensor queue evicted old samples as designed; normal streaming resumed after
the fresh TLS Session was established.

Result: **PASS**

## Defects discovered during physical testing

### Ultra96 retained an incomplete transfer after an ESP rejection

Initial sequence:

1. CRC fault correctly produced ESP result 3.
2. Ultra96 had already received the accepted `FILE_BEGIN`.
3. Ultra96 did not clear that partial state when it received the failed
   `FILE_RESULT`.
4. The following offset-test `FILE_BEGIN` was incorrectly rejected as an
   overlapping transfer.
5. Ultra96 returned protocol error 6, causing an unnecessary TLS reconnect.

Correction:

- Any nonzero ESP `FILE_RESULT` now makes Ultra96 close and delete the partial
  file and clear all active-transfer state.
- An automated regression test confirms that another `FILE_BEGIN` is accepted
  in the same Session immediately after a failed result.

Physical retest:

```text
r -> result=3
o -> result=2
f -> result=0
```

All three completed sequentially without a disconnect or overlapping-transfer
error.

### Windows TLS close produced an expected-reset traceback

The laptop correctly logged `FILE_SEND_ABORTED` and accepted the reconnected
Hub, but Python 3.14's Windows Proactor surfaced `WinError 64` while awaiting
TLS writer closure.

Correction:

- Expected late writer-close errors are suppressed during connection cleanup.
- This affects only shutdown logging; the original reconnect and subsequent
  transfer already succeeded.

## Automated verification

Most recent relevant results:

```text
protocol host tests: PASS
laptop test discovery: 10 passed
Ultra96 server and visualizer-protocol tests: 13 passed
hub3_private_tls (then named hub3_demo_tls): SUCCESS
hub6_private_tls (then named hub6_demo_tls): SUCCESS
hub3: SUCCESS
leaf1: SUCCESS
hub3_private_relay: SUCCESS
hub6_private_relay: SUCCESS
```

The complete Ultra96 discovery suite still requires the optional `websockets`
package for the unrelated visualizer-gateway test, as already recorded in the
Phase 1–6 handoff.

The existing PlatformIO warning that profiles expect 16 MB flash while the
connected chip reports 2 MB remains unresolved. The TLS application still fits
the configured 1 MiB application partition at approximately 880 KiB.

## Phase 7 acceptance summary

```text
64 KiB successful transfer: PASS
256 KiB successful transfer: PASS
Source/ESP/Ultra96 SHA-256 equality: PASS
CRC failure result 3: PASS
Wrong-offset failure result 2: PASS
Timeout result 6 and cleanup: PASS
Mid-transfer disconnect cleanup: PASS
Fresh FILE_BEGIN under a new Session: PASS
Normal 50 Hz coexistence: PASS
Bounded queue and heap behavior: PASS
```

Phase 7 is complete and physically verified on Hub 3.

## Phase 8 starting point

Phase 8 requires two concurrent physical hubs. Before physical testing, extend
the laptop demo controller from its current single-active-hub state to maintain
independent per-hub:

- TLS connections and writers.
- Session IDs and Boot IDs.
- Request sequences, duplicate caches, and pending requests.
- Normal-stream and benchmark statistics.
- File-transfer selection and results.
- Readable Hub 3 and Hub 6 identification in logs.

Then flash `hub3_private_tls` and `hub6_private_tls`, run both normal streams for one
minute, alternate interactive requests, exercise individual and concurrent
benchmarks, and verify that disconnecting or congesting one hub does not close
or block the other.
