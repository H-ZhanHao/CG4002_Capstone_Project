# ESP32 Individual Demo: Phase 9 Physical Test Results and Handoff

> **Historical test record - 28 September 2026, Phase 9.**
> These reset, network-loss and stale-session checks exercised physical Hub 3/6
> with approximately 50 Hz synthetic IMU production over the TLS laptop/SSH route.
> The observed network absence and recovery are not a reconnect-latency guarantee;
> the exact retry-delay transcript was not captured. Paths, commands and the
> Phase 10 starting instructions below belong to the historical revision. See
> [current setup references](../README.md#component-guides),
> [recorded results and limits](../EXPERIMENTS_AND_RESULTS.md#4-concurrent-producers-and-recovery)
> and the retained [Phase 10 outcome](PHASE_10_IMPLEMENTATION_HANDOFF.md).

This work prepared for the Individual Subsystem Test on 1 October; it does
not describe final-product integration.

Date recorded: 28 September 2026  
Repository: `D:\NUS\CG4002-communications`  
Physical devices: FireBeetle Hub 3 and FireBeetle Hub 6

## Purpose

This document records the implementation state and physical verification of
Phase 9 of the [individual-demo milestone sequence](../DESIGN_EVOLUTION.md#1-milestones-and-original-assessment-scope),
now preserved in the consolidated design record.
Read the Phase 1–6,
Phase 7, and Phase 8 handoff documents first for the established TLS topology,
protocol behavior, bounded queues, file transfer, and concurrent-hub evidence.

Phase 9 demonstrates that a single-hub power reset or network loss does not
interrupt the other hub, that recovery establishes a fresh TLS/application
Session, and that benchmark, file, and interactive-request state cannot cross
the Session boundary.

## Physical test results

### Hub 3 power reset while Hub 6 continued

Ultra96 recorded Hub 3 before reset:

```text
HELLO accepted: hub=3 firmware=1.0.0
boot_id=3927540894 session=3405792474
```

After physically resetting only Hub 3, its serial output showed a new boot,
certificate-verified TLS connection, `HELLO`, and Session allocation:

```text
Connecting to 192.168.0.2:15000
TLS connection established
Sending HELLO: hub=3 boot_id=3990631657
HELLO accepted; session=3202014456
```

Ultra96 independently recorded the new identity:

```text
HELLO accepted: hub=3 firmware=1.0.0
boot_id=3990631657 session=3202014456
```

Therefore:

```text
Boot ID changed: PASS
Session ID changed: PASS
```

During Hub 3's restart, Hub 6 retained:

```text
boot_id=3274496861
session=196838934
approximately 49.3 to 51.3 packets/s
gaps=0 duplicates=0 out_of_order=0 timestamp_regressions=0
```

After recovery, Hub 3 settled to approximately 50.3 packets/s and the combined
stream returned to approximately 31.97 kbps and 99.9 packets/s.

The ESP's bounded sensor queue evicted older samples while TLS was unavailable.
This is expected: memory remains bounded and recent samples are retained rather
than allowing an unbounded backlog.

Result: **PASS**

### Network-only loss with no firmware reboot

Hub 3 was kept powered while its Wi-Fi path was unavailable. The laptop
recorded:

```text
16:36:12 Hub 3 ready; session=1847139766; ready_hubs=[3, 6]
16:37:19 Hub 3 disconnected; ready_hubs=[6]
16:38:23 Hub 3 ready; session=3820799023; ready_hubs=[3, 6]
```

Ultra96 recorded:

```text
Before: boot_id=4140072089 session=1847139766
After:  boot_id=4140072089 session=3820799023
```

The unchanged Boot ID proves that the firmware did not reboot. The changed
Session proves that recovery repeated the connection handshake and allocated a
fresh application Session. Hub 6 remained registered while Hub 3 was absent.

The previously implemented reconnect policy uses bounded exponential backoff.
This test physically demonstrated automatic recovery after approximately 64
seconds of network absence, but the individual retry-delay lines were not
captured in the supplied serial transcript.

Result: **PASS** for network-only isolation and recovery.  
Evidence note: exact retry intervals were not retained.

### Interrupted benchmark

A Hub 3 benchmark began under the old Session:

```text
BENCHMARK_START hub=3 sequence=1
BENCHMARK_LIVE hub=3 duration=2.7s frames=9520 average=1121.15kbps
BENCHMARK_LIVE hub=3 duration=7.7s frames=15104 average=625.16kbps
```

Hub 3 was reset before the ten-second benchmark completed:

```text
Hub 3 disconnected; ready_hubs=[6]
```

There was no successful `BENCHMARK_REPORT` or `BENCHMARK_RECEIVED` for the
interrupted Session. Hub 6 continued normal streaming during the initial
failure. Hub 3 later reconnected with a new Session:

```text
session=576861502
```

Benchmark traffic did not resume automatically. A newly requested benchmark
under the fresh Session completed normally.

Result: **PASS**

### Interrupted file transfer

A 256 KiB file was selected to provide sufficient time for physical
interruption. Hub 6 was reset after file progress had begun.

Observed behavior:

- The active send stopped when the old TLS connection was lost.
- The interrupted file ID never produced a successful `FILE_VERIFIED` result.
- Hub 3 continued operating while Hub 6 reconnected.
- No file chunks resumed automatically under Hub 6's new Session.
- Pressing `f` again created a new transfer from offset zero.
- The new transfer completed successfully.

This reconfirms the Phase 7 rule that an interrupted transfer requires a fresh
`FILE_BEGIN`; transfer state is never resumed across Sessions.

Result: **PASS**

### Stale interactive commands after reconnect

Before reset, Hub 6 processed a request under:

```text
session=1685914014 sequence=1
```

After reconnect and before any new request, both `d` and `c` produced only:

```text
No TEST_REQUEST is cached in any current Session
```

No `TEST_REQUEST`, `TEST_RESPONSE`, or hub error was generated by those stale
diagnostic commands.

A fresh request was then sent under:

```text
session=2253957237 sequence=1
```

The new request succeeded, `d` returned its exact cached response, and `c`
returned the expected current-Session replay conflict:

```text
Hub ERROR hub=6 code=8 offending_type=0x20 sequence=1
```

This proves that request caches are cleared at disconnect and cannot be reused
under a new Session, while duplicate/conflict diagnostics still work after new
Session state is established.

Result: **PASS**

## Controller defects found and corrected

### File sending blocked all keyboard input

The original Phase 8 controller awaited the complete paced file transfer inside
the keyboard loop. During a 256 KiB transfer, no further keys were read, making
the interface appear frozen even though `Ctrl+C` still worked at the terminal
level.

Correction:

- File sending now runs as a tracked background task.
- Hub selection, automatic selection, and `q` remain responsive.
- A second file operation on the same hub is rejected with a clear warning.
- Actions targeting a file-busy hub receive a clear warning.
- All-hub benchmark control is rejected until active file operations finish.
- Background file tasks are cancelled and collected during shutdown.

### Invalid file path silently killed the old keyboard task

The 256 KiB test file was located at:

```text
D:\NUS\CG4002-communications\laptop\phase9-test-256k.bin
```

Using `..\phase9-test-256k.bin` from the `laptop` directory pointed to a
nonexistent file. Previously, the resulting read exception occurred before
`FILE_BEGIN` and terminated keyboard processing.

Correction:

```text
FILE_PREPARING hub=<id> path=<resolved absolute path>
FILE_OPEN_FAILED hub=<id> path=<resolved absolute path>: <reason>
```

File-open failure no longer stops keyboard handling. The valid argument for the
physical test was `--file .\phase9-test-256k.bin` or the absolute path.

### Diagnostic key fallback could create an unintended new request

Previously, `d` or `c` with no cached request fell through to the normal-request
branch. This was Session-safe but confusing.

Correction:

- Disconnect clears the global last-request hub reference.
- `d` and `c` with no cache in a current Session emit a warning and transmit
  nothing.

### Fast reconnect produced an ambiguous old-connection message

On a fast reconnect, the new hub connection could be registered before the old
handler finished closing. The ownership guard correctly prevented the old
handler from deleting the new connection, but its generic close message was
ambiguous.

Correction:

```text
Superseded Hub <id> connection finished closing: <reason>
```

This identifies the close as belonging to the replaced connection. A slower
disconnect retains the ordinary explicit transition:

```text
Hub <id> disconnected; ready_hubs=[...]
Hub <id> ready; session=...; ready_hubs=[...]
```

## Automated verification

Latest laptop verification after the reliability-test corrections:

```text
Python compilation: PASS
laptop unittest discovery: 16 passed
git diff --check for laptop files: PASS
```

The added tests verify that file operations are tracked in the background and
that a stale diagnostic key cannot be converted into a new request.

## Phase 9 acceptance summary

```text
Hub 3 power reset with new Boot ID and Session: PASS
Unaffected Hub 6 remains live during Hub 3 reset: PASS
Network-only loss with unchanged Boot ID: PASS
Fresh Session after network recovery: PASS
Unaffected hub remains registered during network loss: PASS
Automatic reconnect after returning to range: PASS
Exact exponential retry-delay transcript: NOT CAPTURED
Interrupted benchmark does not resume: PASS
Fresh benchmark works after reconnect: PASS
Interrupted file transfer does not resume: PASS
Fresh FILE_BEGIN and offset-zero retry: PASS
Stale request cache is not reused after reconnect: PASS
Per-hub state and connection ownership remain isolated: PASS
```

Phase 9 reliability behavior is implemented and physically verified on Hubs 3
and 6. The only evidence limitation is that the exact reconnect-delay sequence
was not retained, although automatic network recovery and the bounded-backoff
implementation were both present.

## Phase 10 starting point

Phase 10 joins the established communications path to the existing visualizer
publisher and secure WebSocket gateway:

```text
laptop keystroke
  -> randomly selected valid TEST_REQUEST
  -> selected FireBeetle transforms the IMU payload
  -> correlated TEST_RESPONSE travels through the laptop
  -> Ultra96 generates a random AI event
  -> existing visualizer publisher and WSS gateway
  -> phone displays the event
```

Required work and tests:

1. Inspect the current Ultra96 visualizer publisher/gateway and phone-facing
   event schema before changing the verified server path.
2. Generate exactly one correctly formatted random AI event for each newly
   accepted `TEST_RESPONSE`.
3. Preserve Hub ID, Session, Sequence, and transformed readings in logs so the
   request, response, Ultra96 event, and phone display can be correlated.
4. Ensure an exact duplicate response does not publish a second AI event.
5. Ensure malformed, conflicting, stale-Session, or unauthorized responses do
   not publish any event.
6. Run the complete path with random payload and random target-hub selection.
7. Verify the phone displays the resulting event while both normal 50 Hz
   streams remain healthy.

Phase 10 must preserve all existing TLS, bounded-queue, benchmark, file, and
two-hub reliability behavior.
