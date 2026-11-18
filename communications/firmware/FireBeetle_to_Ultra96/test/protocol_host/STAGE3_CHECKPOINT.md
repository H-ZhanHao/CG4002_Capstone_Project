# Stage 3 checkpoint: phone-owned rep feedback

> **Historical checkpoint - Stage 3 phone-owned rep feedback.**
> This 100 Hz integration routed phone/simulator reps to the earlier LED-flash
> outputs. The user confirmed the listed physical simulator scenarios; the real
> AR teammate counter/updated APK and Unity device lifecycle remained unverified
> at this checkpoint. Managed/loopback tests do not establish those device checks.
> Commands, API examples, deployment lists and next-stage references below apply
> to the matching saved Stage 3 source/images, not the final output contract.
> See [current setup references](../../../../docs/README.md#component-guides) and
> [the consolidated checkpoint results](../../../../docs/EXPERIMENTS_AND_RESULTS.md#6-feedback-idle-and-fall-checkpoints).

Stage 2 physical behavior was confirmed by the user. Stage 3 is implemented and
automated checks pass. The user confirmed physical simulator cases for duplicate
counts, count jumps, reconnect synchronization, new sessions and a disconnected
node all passed. The AR teammate's actual counter/updated APK remains untested.
Stage 4 is now documented separately in STAGE4_CHECKPOINT.md. No boards were flashed, files deployed
to Ultra96, commits created, or external AR project files edited.

## Implementation and retained builds

The existing WSS connection now carries phone requests and delivery receipts
alongside exercise updates:

```text
Phone / simulator -- existing pinned WSS --> visualizer_gateway.py
                 -- loopback TCP 9101 --> server.py OR server_AI.py
                 -- existing TLS + Stage 2 FEEDBACK_COMMAND --> both ESP LEDs
```

The gateway owns no ESP socket. Both server entry points start the same rep
control service, using the visualizer publisher's current `run_id`. AI inference
and its intended `window_assembler_updated` import remain unchanged.

New `node1_stage3` / `node2_stage3` environments and matching sdkconfig files
inherit the Stage 2 settings. The ESP protocol and output implementation are
unchanged from Stage 2. No stage fallback flags were added. Previous sdkconfig
files and environment definitions are retained unchanged.

The working Stage 2 source, Unity assets, configuration files, flash scripts,
and images were saved before editing under repository root
`.build/firmware_stages/stage2_verified_before_stage3/`. Stage 3 source and flash
bundles are under `.build/firmware_stages/stage3_2026-11-01/`. These archives are
independent of PlatformIO's disposable `.pio/build` cache. Earlier source must
be rebuilt from its matching snapshot; saved binaries can be flashed directly.

## Phone contract, version 1

The server sends `rep_welcome` after the gateway attaches the phone's new
loopback link:

```json
{"v":1,"type":"rep_welcome","run_id":12345678,"requires_sync":true}
```

Do not derive the current rep run from an old cached exercise update. Wait for
this welcome. Every phone command has exactly these seven fields:

```json
{"v":1,"type":"rep_event","run_id":12345678,"seq":2,"rep_session_id":"setA","rep_index":1,"exercise":"squat"}
```

| Field | Validation |
| --- | --- |
| `v` | Integer 1 |
| `type` | `rep_session_start`, `rep_sync`, or `rep_event` only |
| `run_id` | Current server run; integer 1..9007199254740991 |
| `seq` | Increasing integer 0..9007199254740991 within this phone connection |
| `rep_session_id` | Fresh set/reset identity: 1..64 ASCII letters, digits, `_`, `-` |
| `rep_index` | Cumulative integer 0..1000000000 |
| `exercise` | `squat`, `push_up`, `sit_up`, `lunge`, `leg_raise`, `jumping_jack` |

Booleans are not integers. Unknown fields, unsupported commands, bad types,
and messages over 1024 UTF-8 bytes are rejected. Phone START/STOP, idle buzzer,
fall injection, and arbitrary hardware commands are not exposed.

- `rep_session_start`: explicitly starts/resets a set using a new identity and
  count baseline. Starting at baseline 0 or a current count never flashes.
  Previously used identities cannot be reactivated in the same server run.
- `rep_sync`: mandatory when reconnecting to the same set. Its count must be at
  least the server's current count. It establishes the latest count without
  feedback, including reps counted while disconnected. A new process/run needs
  an explicit fresh session start; old-run requests are rejected.
- `rep_event`: enabled only after start/sync on this connection. A new higher
  count requests one REP_LED event on both nodes. A jump 4 -> 7 gives one pulse,
  not three delayed pulses. A previously accepted count is a duplicate even
  with a new phone sequence; its original feedback event ID is retained. A
  decreasing, unknown count is rejected and never treated as an automatic reset.

The server maintains 64 sequence fingerprints per connection and 128 recent rep
receipts. Reusing a cached sequence with different contents is rejected. Evicted
old sequences/counts fail closed without another pulse. Retired session IDs are
bounded to 256 per run; capacity exhaustion rejects new sets until server restart.

## Results and reconnect behavior

`rep_result` echoes `run_id`, `seq`, `rep_session_id`, and `rep_index`, with:

- `status`: `session_started`, `synced`, `accepted`, `duplicate`, or `rejected`.
- `reason`: readable acceptance/rejection reason.
- `feedback_event_id`: Stage 2 logical ID, or 0 for a baseline/rejection with no
  feedback event.
- `nodes`: per-node objects containing `node_id`, `status`, and `result`.

Node statuses include `pending`, `applied`, `rejected`, `timeout`, `unavailable`,
`disconnected`, `send_failed`, `queue_full`, `cancelled`, and `expired`. Result
is the Stage 2 numeric firmware ACK code, or -1 when no ACK is available.

If delivery is still pending in the initial result, an asynchronous
`rep_delivery` follows with `status: complete`, the same identities, and final
node outcomes. If both nodes already finished, the initial result contains
those outcomes and no second message is needed. `accepted` means the rep was
accepted for delivery, not that either physical LED worked. Applied outcomes
still require physical observation.

Feedback uses the same logical event ID through Stage 2 retries. Delivery
tickets are bounded to 128, independent of later console/phone commands. On
phone disconnect, link replacement, set reset, or explicit sync, unsent AR
feedback/retries are cancelled. Frames already inside a transport send are in
flight and cannot be undone. Reconnect never replays a queued rep history.

## Ownership and bounds

The loopback service defaults to `127.0.0.1:9101`, accepts at most four links,
and only its newest link can submit reps. Per-link input is limited to 20 phone
messages/second and 16 outstanding delivery notifications. A single handler
serializes local result writes. Configure server `--rep-control-port` and
gateway `--control-port` together if changing the port. Do not expose it through
the phone relay or bind it externally.

The gateway limits WSS input to 1024 bytes and eight queued incoming messages,
and keeps at most 32 reliable outgoing receipts. Malformed messages also consume
the rate budget; exceeding 20 messages/second closes WSS with code 1008.
Receipt overflow closes the
phone connection so it must reconnect/sync. Exercise updates retain their
independent one-slot latest-value buffer. Exactly one WSS task writes both
streams; exercise throttling does not delay rep receipts.

Unity client writes are masked and bounded to 1024 bytes, with one lock shared
by text and pong frames. Client main-thread callbacks are bounded to 128; overflow
disconnects instead of accumulating a replay backlog. Only one rep request send
is active at a time; disconnected/busy calls are not queued.

## Unity API handoff

The checked-in communications demo exposes these methods on
`Ultra96VisualizerClient`, called on Unity's main thread:

```csharp
await client.BeginRepSession("squat", baselineCount: 0); // new ID for a new/reset set
// Wait for client.RepSessionReady (or the session_started receipt).
await client.SendRepEvent(arCumulativeCount);            // AR owns the algorithm
// Optional explicit baseline sync, without a flash:
await client.SyncRepSession(arCurrentCount);
```

Subscribe to `client.RepFeedbackReceived` for `RepFeedbackResponse` results and
per-node `Nodes`. Method `true` returns mean a request was written to WSS, not
accepted/applied by the server or boards. New sends are enabled by start/sync
ACKs. Reconnect automatically syncs the retained local count; a changed run
creates a fresh session with the current baseline. No disconnected reps are
queued. A lower count requires `BeginRepSession` with a fresh identity.

This does not integrate or modify the teammate's external AR application. They
must call the API from their actual rep counter and reset handler.

## Automated verification

| Check | Result |
| --- | --- |
| Stage 1/2 firmware host regression suites | PASS |
| Existing Python regression suite | 129 tests, OK; one optional AI configuration subtest skipped |
| Stage 3 tests | 24 PASS, across both server entry points |
| Managed C# protocol cases | 13 PASS; all networking sources compile against installed Unity assemblies |
| Actual checked-in C# WSS transport -> gateway -> IPC -> two production-C peers | PASS for both server entry points |
| Simulator CLI -> WSS -> IPC -> two production-C peers | PASS for both server entry points |
| `node1_stage3` | PASS; RAM 77,864 bytes; firmware flash 867,339 bytes |
| `node2_stage3` | PASS; RAM 77,864 bytes; firmware flash 867,343 bytes |

Coverage includes new sets, baseline sync, duplicate counts/new sequences,
delayed duplicates, conflicting sequences, count jumps, stale run/session/link,
reconnect without replay, reset, unavailable node, timeout, retry cancellation,
request rate limits, cache capacity, oversized WSS input, concurrent exercise
updates, and serialized WSS writes. TLS tests use generated certificates; the
Windows C# TLS tests required unsandboxed access to OS TLS credentials.

Unity's batch Editor test attempt was blocked by an unavailable Editor license.
Managed compilation/protocol tests and the real C# transport tests passed, but
Unity scene lifecycle and Android/AR runtime behavior still need testing in a
licensed Editor/device. The standalone managed compile reports reference-version
compatibility warnings for Unity's netstandard assemblies. AI inference itself
was not run; the intended external window module is represented only by a
temporary test fixture in AI feedback tests.

From repository root with GCC/G++, .NET, installed Unity references, and the
Python requirements available:

```powershell
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage3_tests.ps1
```

The local runtime used here sets `PYTHONPATH` to `.build/stage2_python_deps` and
passes `-PythonCommand D:/PlatformIO/penv/Scripts/python.exe`. `-SkipManaged` can
skip managed compilation on machines without Unity references; C# transport
tests then skip if their host executable is unavailable. Actual Unity EditMode
tests are also supplied in `Assets/Tests/EditMode/RepFeedbackProtocolTests.cs`.

## Physical checkpoint

> **Status clarification:** Simulator duplicate/count-jump/reconnect/new-session
> and absent-node cases were user-confirmed as stated above. The real AR caller/
> APK acceptance requested here remained unverified in this record. The original
> per-board LED-pulse expectations were superseded by the final Hip rep beep.

1. Save/flash only the matching Stage 3 bundles, using the archive scripts from
   repository root and actual COM ports:

   ```powershell
   & .build/firmware_stages/stage3_2026-11-01/node1_stage3/flash.ps1 -Port COM_HIP
   & .build/firmware_stages/stage3_2026-11-01/node2_stage3/flash.ps1 -Port COM_WRIST
   ```

2. Deploy these updated files together to the existing Ultra96 communications
   directory: `server.py` and/or `server_AI.py`, `feedback_delivery.py`,
   `rep_control.py`, `visualizer_protocol.py`, and `visualizer_gateway.py`.
   Deploy `simulate_phone_reps.py` to the machine running the simulator.
   Existing TLS/SSH/laptop relay and certificate provisioning remain unchanged.

3. Run exactly one sensor server, with its existing dependencies:

   ```sh
   python3 server_AI.py --sample-rate-hz 100 --rep-control-port 9101
   # Or use server.py for the communications-only server.
   ```

   Start the gateway in a second terminal using the existing certificate/key:

   ```sh
   python3 visualizer_gateway.py --tls-cert /path/to/server-cert.pem --tls-key /path/to/server-key.pem --control-port 9101
   ```

   The new control port remains local to Ultra96. No new tunnel/relay port is
   needed. Confirm the gateway and server are from the same Stage 3 source.

4. Stop the existing Unity client while using the simulator: only one WSS
   visualizer is supported at a time. On the laptop/phone-accessible machine,
   use the existing WSS endpoint and its trusted public certificate PEM:

   ```sh
   python simulate_phone_reps.py --url wss://LAPTOP_IP:9000 --ca-cert /path/to/server-cert.pem --exercise squat --counts 1,1,4
   ```

   Replace `LAPTOP_IP` with the host/IP covered by the certificate. TLS
   verification is required. Expect one flash per board for count 1, no flash
   for duplicate 1, and one flash for jump to 4: **two pulses per board total**.
   Session start must not flash. Inspect JSON results for accepted/duplicate
   and each node's applied/timeout/unavailable outcome.

5. Record the printed `rep_session_id`, then reconnect to that same set with a
   sync baseline of 4 (replace the ID):

   ```sh
   python simulate_phone_reps.py --url wss://LAPTOP_IP:9000 --ca-cert /path/to/server-cert.pem --exercise squat --sync --session-id PRINTED_ID --baseline 4 --counts 4,5
   ```

   Sync and duplicate count 4 must not flash. New count 5 gives one pulse per
   board. To simulate offline count progress, sync to baseline 10 and send
   counts `10,11`: only 11 should pulse. A new default invocation creates a new
   session; starting again at count 0 and sending 1 should work as a set reset.

6. Disconnect one ESP's network and submit new reps: the connected board should
   still pulse while the other outcome reports unavailable/disconnected/timeout.
   Reconnect it: no old rep should replay. Repeat after server console `stop`;
   outputs should work while acquisition/history continue. Use `start` and move
   a sensor to verify normal streaming resumes.

7. Integrate the teammate's Unity caller with BeginRepSession/SendRepEvent and
   the explicit reset API. Verify the same results on the real phone, including
   receiving exercise updates while sending reps, and disconnect/reconnect.

## Changed areas

- `visualizer_protocol.py`: strict phone contracts alongside unchanged exercise
  updates; `rep_control.py`: session, replay, IPC, and delivery notifications.
- Both servers: local rep service lifecycle and current run identity.
- `feedback_delivery.py`: per-event outcomes and AR retry cancellation.
- `visualizer_gateway.py`: bidirectional WSS, bounded IPC forwarding, reliable
  receipts, and single writer separate from the latest exercise slot.
- Unity networking: rep contract/API and masked, serialized text writes;
  protocol tests plus a managed host for compilation and real WSS validation.
- Simulator, integration tests, test runner, new profile pairs, and docs.

The user authorized Stage 4 after confirming simulator physical results; actual
AR caller integration remains with the Unity teammate and is not claimed tested.
See STAGE4_CHECKPOINT.md for the next physical checkpoint.
