# Stage 2: TLS feedback commands

> **Historical checkpoint - Stage 2 TLS feedback commands.**
> This 1 November 2026 implementation used 100 Hz acquisition and the then-current
> feedback actions: rep/ready LED flashes and a Hip idle beep. The opening update
> records the user's later Stage 2 confirmation and authorization for Stage 3;
> the original pending/Stage-3-not-started text below describes the earlier gate.
> No confirmation date beyond the existing record is inferred. Tests/counts and
> ACK semantics remain specific to this revision. Commands, deployment lists
> and closing checkpoint instructions require matching historical source/images.
> See [current setup references](../../../../docs/README.md#component-guides) and
> [the consolidated checkpoint results](../../../../docs/EXPERIMENTS_AND_RESULTS.md#6-feedback-idle-and-fall-checkpoints).

Historical checkpoint: the user confirmed the Stage 2 commands and authorized
Stage 3. Verified Stage 2 source/images are retained under
`.build/firmware_stages/stage2_verified_before_stage3/`. See
`STAGE3_CHECKPOINT.md` for current phone rep routing and physical tests.

Implemented on 2026-11-01. Stage 1 physical behavior was confirmed by the user.
Stage 2 automated checks and both new firmware builds pass; physical testing is
pending. Stage 3 has not started. No boards were flashed, no Ultra96 files were
deployed, and no commits were created by this workflow.

Follow-up: `server_AI.py` now has the same Stage 2 manual feedback integration
as `server.py`. Its intended `window_assembler_updated` reference, inference
entry points, and existing idle classification handlers are unchanged. All 20
feedback tests pass across both servers. The full updated Python suite runs
129 tests successfully with one optional AI configuration subtest skipped.
AI-server feedback tests use a
test-only window-module stand-in and do not run inference. Updated Ultra96
source is additionally archived under
`.build/firmware_stages/stage2_ai_update_2026-11-01/`; Stage 2 firmware is unchanged.

## Scope and retained versions

- Removed `APP_STAGE1_FEEDBACK` source guards and all legacy LED writes/ready/rep
  triggers in the communications `exercise_bridge.cpp`. Independent feedback is
  now the active implementation; no Stage 2/3/4 fallback branches were added.
- Every pre-existing sdkconfig file and PlatformIO environment section is
  retained unchanged. The obsolete `APP_STAGE1_FEEDBACK` setting remains only
  in the retained Stage 1 environment definitions, where it is now inert.
  `APP_FEEDBACK_DIAGNOSTICS` is an ordinary USB diagnostics option, not a stage
  fallback selector.
- Only `node1_stage2` / `node2_stage2` were built. Each has its own matching
  `sdkconfig.node1_stage2` / `sdkconfig.node2_stage2`, using the same 100 Hz,
  physical IMU, TLS, and idle-resume settings as the working Stage 1 profiles.
- Verified Stage 1 flash files, configurations, a source snapshot, manifests,
  and manual flash scripts were saved before editing under repository root
  `.build/firmware_stages/stage1_verified_2026-11-01/`.
- Stage 2 flash files and a matching source snapshot are under
  `.build/firmware_stages/stage2_2026-11-01/`. Both archives are separate from
  `.pio/build`; use their flash scripts to flash saved versions without rebuilding.
  Earlier versions must be rebuilt from their matching source snapshots.
  PlatformIO cleared earlier `.pio/build` environment folders during the new
  build; their three flash files were restored byte-for-byte from the saved
  archives. Treat `.pio/build` as a disposable cache and use the archives for
  dependable version retention.
- Standalone `hardware/` remains unchanged. AR rep routing, AI-triggered idle
  feedback, and real fall detection/reporting are outside this stage.

## Wire contract

Unused message ID `0x0B` is now `FEEDBACK_COMMAND`. The existing 24-byte header
and framing remain unchanged. Coordinator-originated feedback requires node=0,
flags=0, the current nonzero transport Session, and the command sequence.
Multibyte values use network byte order (big endian).

| Payload offset | Width | Meaning |
| --- | --- | --- |
| 0 | 8 bytes | Nonzero uint64 logical event ID |
| 8 | 1 byte | Action: 1 `REP_LED`, 2 `READY_LED`, 3 `IDLE_BUZZ` |
| 9 | 7 bytes | Reserved, must all be zero |

Total payload length is exactly 16 bytes. Python format is `!QB7x`. Event IDs
increase within each transport Session. Both node deliveries share one event
ID; each node uses its current Session. A retry preserves the same frame,
including event ID, action, sequence, and header timestamp. Header timestamps
are diagnostic and are not part of logical request contents or an ESP expiry
clock. Firmware does not require clock synchronization to apply feedback.

`CONTROL_ACK` retains its existing 16-byte `!BBHIQ` payload:
command type (1 byte), result (1), zero reserved (2), command sequence (4),
effective local microsecond timestamp (8). Its header sequence must match the
acknowledged command sequence, its node must match the connection, and its
Session must be current. For feedback, command type is `0x0B`.

| Result | Meaning for feedback |
| --- | --- |
| 0 `APPLIED` | Accepted and applied/scheduled by the pipeline |
| 2 `INVALID_STATE` | Malformed request/header or unavailable physical pipeline |
| 3 `DUPLICATE` | Previously accepted request; no second output |
| 5 `STALE_IDENTITY` | Wrong Session or an old, evicted sequence/event ID |
| 6 `BUSY` | Queue full or controller rejects a conflicting output request |
| 7 `UNSUPPORTED` | E.g. idle buzzer requested on Wrist |
| 8 `CONFLICT` | Cached sequence/event ID reused with different request contents |

Unknown action values, zero event IDs, wrong lengths, and nonzero reserved bytes
fail payload validation and receive a negative ACK. Results 1 and 4 retain their
capture-control meanings. ACK acceptance is not physical proof of GPIO output.

## Ownership, duplicate suppression, and delivery bounds

The application task validates and submits feedback to the pipeline's existing
8-entry command queue without waiting. Only the pipeline applies output state
and maintains feedback replay state. A separate persistent 8-entry result queue
returns ACKs asynchronously; no per-request reply queue is destroyed while a
pending feedback command could still reference it.

The 32-entry firmware cache checks both sequence and logical event identities,
including delayed duplicates after other commands and logical retries using a
new transport sequence. Cached results preserve the original effective timestamp.
Rejected cached requests stay rejected. Sequence/event high-water marks reject
evicted old requests instead of permitting replay. They survive STOP/ARM/START
and reset on Session changes. Do not wrap or reset the uint64 event counter or
uint32 feedback transport sequence inside a Session; reconnect before exhaustion.

Ultra96 allows up to 8 pending deliveries and 8 queued writes per node, with
independent sender workers and serialized socket writes. Feedback ACKs do not
consume or alter pending START, STOP, or ARM acknowledgements. A blocked or
disconnected node does not prevent the other node's feedback sender from running.

Delivery uses at most 3 attempts, 750 ms retry spacing, and a 2.5 second server
deadline. No retry is queued after expiry. An already in-flight packet can still
arrive; its eventual ACK cannot turn a timed-out server result into success.
Negative ACKs terminate delivery rather than replaying a rejected output later.
Disconnect/replacement clears that Session's pending delivery and queued work;
old requests are never replayed into a reconnect. Completed history is bounded
to 128 results. Console status exposes the latest event, with logs for results,
unavailable targets, retries, timeouts, and ignored late ACKs.

## Automated results

| Check | Result |
| --- | --- |
| Seven Stage 1 host test executables | PASS |
| C feedback codecs, independent expected ACK bytes, replay engine | PASS |
| Python protocol/coordinator/window/gateway suite | 119 tests, OK; 1 AI-only subtest skipped |
| `node1_stage2` | PASS; RAM 77,864 bytes; firmware flash 867,339 bytes |
| `node2_stage2` | PASS; RAM 77,864 bytes; firmware flash 867,343 bytes |

New coverage includes the production C engine behind two simulated ESP peers,
lost ACK/retry with one actual apply, delayed duplicates, conflicting identities,
cache eviction, malformed fields, stale Sessions, independent node results,
disconnect/reconnect, bounded pending queues, a stalled sender, exactly three
attempts, deadline expiry, and console controls. Existing acquisition and
coordinator control-flow regression checks pass.

The AI-only subtest is explicitly skipped because existing `server_AI.py`
imports the intended external `window_assembler_updated.py`, unavailable in the
local test environment. That import is not modified or replaced in production;
the separate AI feedback tests use a temporary test-only stand-in. Gateway tests
use the project's pinned `websockets==12.0`; the local test dependency was
installed into `.build/stage2_python_deps` using PlatformIO's Python 3.11.

Run normally from repository root with GCC/G++ and the Python requirements:

```powershell
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage2_tests.ps1
```

Exact local-runtime variant used here:

```powershell
$env:PYTHONPATH = "$PWD/.build/stage2_python_deps"
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage2_tests.ps1 -PythonCommand D:/PlatformIO/penv/Scripts/python.exe
```

From the firmware project, build only the new profiles:

```powershell
pio run -e node1_stage2 -e node2_stage2
```

## Physical checkpoint: user execution only

> **Status clarification:** The opening historical update records that these
> Stage 2 commands were subsequently user-confirmed. The procedure and expected
> output patterns below belong to the saved Stage 2 revision. They do not verify
> final output behavior or turn automated lost-ACK tests into physical injections.

1. Flash each saved Stage 2 bundle onto its matching board. From repository root,
   replace the placeholders with the actual COM ports:

   ```powershell
   & .build/firmware_stages/stage2_2026-11-01/node1_stage2/flash.ps1 -Port COM_HIP
   & .build/firmware_stages/stage2_2026-11-01/node2_stage2/flash.ps1 -Port COM_WRIST
   ```

2. Copy updated `communications/ultra96/server.py` and the new
   `communications/ultra96/feedback_delivery.py` together into the existing
   Ultra96 communications directory using your normal deployment process.
   Keep the established ESP TLS/relay setup. Run `server.py` in the foreground
   with its existing dependencies available:

   ```sh
   python3 server.py --sample-rate-hz 100
   ```

   You can instead run `python3 server_AI.py --sample-rate-hz 100` with its
   intended Ultra96 inference dependencies. Deploy updated `server_AI.py` and
   `feedback_delivery.py` together in that case. Both entry points expose the
   same manual feedback commands; this follow-up does not add automatic idle
   beeps to the AI classification handler.
   Wait for both node HELLO connections. Feedback can work before clock sync,
   while capture is armed, and while transmission is stopped.

3. Type the following commands into the **Ultra96 server terminal**, pressing
   Enter and waiting for each pattern to finish:

   | Command | Expected physical result | Expected server result |
   | --- | --- | --- |
   | `feedback rep` | Both LEDs pulse 200 ms | `FEEDBACK_RESULT` applied on each connected node |
   | `feedback ready` | Both LEDs blink three times, 100 ms off/on | Applied on each node |
   | `feedback idle` | Hip beeps 100 ms, pauses 100 ms, beeps 100 ms | Applied on Hip; no Wrist command |
   | `feedback status` | No new pattern | Latest per-node event status/result |

   Firmware acceptance is not proof of a physical output. Confirm both LEDs
   and Hip buzzer yourself. Wait at least one second between different patterns
   so a ready request is not intentionally rejected during a rep pulse.

4. Paste these two lines together to test duplicate suppression within the
   deadline:

   ```text
   feedback rep
   feedback repeat
   ```

   Each node must produce one 200 ms pulse. The repeated frame must not extend
   or restart it. A retry after the first ACK may log an ignored late ACK with
   result 3 (`DUPLICATE`). For a delayed duplicate, send `feedback rep`, then
   send `feedback repeat` about one second later: no new flash should occur.
   After 2.5 seconds, repeat should report zero queued requests.

5. Enter `stop` in the server terminal, wait for STOP ACKs, and repeat the three
   output commands. Outputs and heartbeats should still work; local acquisition
   and retained history continue. Enter `start` and move a sensor to resume
   trigger-driven streaming; verify normal 100 Hz three-second window reports.

6. Disconnect one board's network and repeat `feedback rep`. The connected
   board must still pulse and ACK. The other target should report unavailable,
   disconnected, send failure, or timeout depending on when its loss is noticed.
   Reconnect it: no old pulse should replay. A new `feedback rep` should reach
   both boards again. Physical lost-ACK retry injection is not provided; lost-ACK
   handling is covered by the production-C simulated-peer automated test.

USB Stage 1 commands remain available on each board. Network `feedback idle`
is a manual output test; the existing `idle` command still drives the original
STOP/ARM flow and does not automatically beep in this stage. Real AI idle
notification belongs to Stage 4. Network fall-alarm commands are not introduced.

## Changed files

- Firmware `include/protocol.h`, `src/protocol.c`: message ID, codec, ACK results.
- `include/feedback_delivery.h`, `src/feedback_delivery.c`: pure C replay engine.
- `include/physical_pipeline.h`, `src/physical_pipeline.c`: request/result queues
  and pipeline-only feedback application.
- `src/application.c`: validation, dispatch, and asynchronous feedback ACKs.
- `components/hardware_core/src/exercise_bridge.cpp`, `include/app_config.h`:
  remove Stage 1 source guards and unused exercise LED ownership.
- `platformio.ini`, `.gitignore`, both new sdkconfig files: new profiles only.
- Ultra96 `feedback_delivery.py`, `server.py`: codec, bounded delivery, independent
  ACK routing, console tests, and serialized protocol-error writes.
- `test_feedback_delivery.c`, `feedback_peer.c`, `run_stage2_tests.ps1`,
  Ultra96 `test_feedback_delivery.py`: checks and production-C peer simulation.
- Ultra96 `test_server.py`: explicitly skip the unavailable optional AI subtest
  while running communications coordinator tests.
- Firmware host README, Stage 1 historical note, this checkpoint, Ultra96 README,
  and implementation plan: stage status, commands, retained profiles/artifacts.

Stop here for the user's physical test result and instruction. Stage 3 has not
started.
