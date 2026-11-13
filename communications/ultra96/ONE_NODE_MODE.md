# Single-node startup and dynamic joining

Implemented on 2026-11-06 as the only server capture behavior. Either node can
start the system, and a partner can join later. No mode flag is required;
`--one-node` has been removed. Existing PlatformIO profile definitions are
unchanged; use `node1_one_node` and `node2_one_node` for live joining, gated by
`APP_ONE_NODE=1` in firmware.
Neither board has been flashed as part of this change.

## Build and run

From the repository root, build both new firmware environments:

```powershell
pio run -d communications/firmware/FireBeetle_to_Ultra96 -e node1_one_node -e node2_one_node
```

The profiles inherit the corresponding Stage 5 physical 100 Hz TLS build,
including idle resume, feedback diagnostics, and hip fall reporting. They use
separate PlatformIO build directories and generated SDK configurations.
Build output is under `.pio/build/node1_one_node` and `.pio/build/node2_one_node`
inside the firmware project. Flashing is a separate deployment step.

On Ultra96, from `communications/ultra96`, use either:

```bash
# Communications capture/window verification, without model execution:
python server.py --max-missing-fraction 0.03

# AI capture, assuming the existing deployed infer_window supports either node:
python server_AI.py --max-missing-fraction 0.03
```

Keep the existing relay and network/TLS setup. Both launch commands always
use independent node readiness, dynamic joining, and available-node windows.
The legacy coordinator that waited for both nodes has been removed. The new
firmware also accepts the existing history-flush commands.

`server_AI.py` imports the AI teammate's `inference_pipeline.InferencePipeline`.
Keep that file on Ultra96; `dynamic_inference.py` is no longer imported by the
server and is not required for deployment. The teammate owns adapting their
pipeline for available-node windows and idle handling. The local
`dynamic_inference.py` remains a reference implementation, not the deployed
pipeline.

`server_AI.py` still needs the deployed model runtime, compiled models, and DPU
overlay for real inference. Their absence in this checkout does not block
communications implementation. Per the agreed assumption, the deployed
`infer_window(window)` accepts node 1 alone, node 2 alone, or both. This assumption
has been tested with deterministic model results, not real model inference.

An optional `--inference-adapter package:factory` loads an alternative adapter.
The factory receives parsed CLI arguments, initializes/retains any required
hardware resources, and returns an object implementing `infer_window(window)`.
It is optional; the normal AI launch uses the existing VART runtime.

## Implemented lifecycle

### Node connection LEDs

The `node1_one_node` and `node2_one_node` builds use the following nonblocking
LED patterns. Existing firmware profile definitions and LED behavior are
preserved because the connection indication is gated by `APP_ONE_NODE`.

| State | LED |
|---|---|
| Booted and trying to reach the server, including Wi-Fi/TLS/HELLO retries | Repeat 500 ms ON, 500 ms OFF |
| Valid Ultra96 `HELLO_ACK` accepted | Three fast 100 ms ON pulses separated by 100 ms OFF |
| Connected, after the burst, waiting for activity | OFF |
| Streaming acknowledged and activity feedback received | Steady ON, after any connection burst finishes |
| Application connection lost | Resume slow blinking |

The connection burst starts with a 100 ms OFF separator so its first pulse is
distinct even if acceptance arrives while the slow blink is ON. Including the
separator and final gap, the burst takes 700 ms. SYNC or ARM does not trigger
another burst. STOP/idle updates the eventual activity state without truncating
the burst. Each newly accepted session gets its own confirmation; a stale
session's teardown cannot change the current session's LED.

The existing application BEGIN/END events carry the validated-handshake and
disconnect state to the physical pipeline, which remains the only owner of
the feedback controller and GPIO. Timing is serviced by its existing loop,
including when sensor reads fail; no blink task or blocking delays were added.
Rep beeps and the hip's fall-alarm priority remain independent of LED timing.

Host verification is in
[`test_connection_led.cpp`](../firmware/FireBeetle_to_Ultra96/one_node_tests/test_connection_led.cpp).
It exercises both node roles, pulse counts/boundaries, startup retry blinking,
streaming handoff, idle, reconnection, timer rollover, legacy LED behavior,
and fall/buzzer preservation. Both node firmware profiles were rebuilt
successfully; a physical LED check after flashing remains required.

### Capture and node availability

- Either synchronized node is armed without waiting for its partner.
- A valid peak from an acknowledged arm starts the available ready producers.
- Each producer's ARM/START ACK is handled independently; the first successful
  START ACK opens the run's assembler once.
- A late synchronized node starts live, without a new peak or a global reset.
- Per-node arm generations and boundaries prevent arming a partner from
  invalidating the first node's trigger.
- Disconnection, failed commands, and stalled samples isolate that producer.
  A remaining stream continues on the same run/window timeline.
- Losing all streaming/starting producers ends the capture. Reconnection arms
  a new capture and requires a fresh peak unless manually stopped.
- Manual STOP persists across reconnects. Idle pause stops transmission and
  rearms nodes independently after their STOP ACKs.
- Clock synchronization math, mapping quality checks, and periodic refresh
  remain in `NodeControlConnection`.

Commands use the existing control ACK timeout (currently three seconds).
A streaming producer with no accepted samples for three seconds is removed and
its transport shut down so the normal reconnect path can recover it. Other
producers continue. Timed-out command transports are also closed; they cannot
later start streaming under an abandoned command.

## Live-join protocol

Only the new firmware profiles accept a `START_FLUSH` with
`history_duration_us = 0` as a live-only start. The board starts from its next
acquisition sequence and reports the actual local boundary in the ACK.
No sensor samples are invented and no pre-boot history is requested.

Initial peak startup still requests the existing pre-peak history. If that
request is rejected (for example, not enough history after boot), the server
retries that producer once with a live start. A failed retry removes that
producer without cancelling another usable stream. The run origin remains
the original mapped peak minus the requested history duration; insufficient
early windows are rejected until real coverage is available.

## Window and AI contract

`samples_by_node` contains only the usable nodes for that window:

| Keys | Input |
|---|---|
| `{1}` | Hip/waist only |
| `{2}` | Wrist only |
| `{1, 2}` | Both nodes on the shared capture timeline |

`quality.available_nodes` identifies those nodes. `sample_counts`, padding and
drop counts remain available for both supported nodes;
`node_rejection_reasons` explains unusable coverage. Small packet-loss padding
retains the existing policy, but a missing producer is never zero-filled.
No usable producer means no accepted inference window.

A known starting or lagging producer receives a bounded capture-time grace
period. The other stream advancing 250 ms past the window end releases the
window without waiting longer. Configure this with `--node-wait-ms`; zero
disables the grace period. It is a timestamp-progress bound, not an extra
wall-clock sleep. Previously emitted windows are not replayed when a node joins.

The AI teammate's pipeline should call `infer_window(window)` for every accepted
input combination. Result fields are the existing pipeline contract:

```text
capture_run_id, window_id, start_us, end_us,
class_id, exercise, confidence, model_window_count
```

The local reference worker validates window/run identifiers and intervals,
uses a bounded queue and capture-generation checks, and retains the recorded
Stage 4 policy: confidence >= 0.80, two exercise evidence scores, and five
consecutive confident idle results. It resets evidence when available inputs
change. These are integration requirements to review with the AI teammate;
their external pipeline implementation has not been verified in this checkout.
Missing windows must never count as idle classifications.

## Validation and remaining work

After removing the legacy mode, 45 dynamic/default-startup tests pass. They
exercise both servers without a mode flag, public coordinator construction,
CLI rejection of the removed flag, independent startup, joining, window
fallback, STOP/idle handling, and inference routing. Firmware was unchanged by
this server cleanup.

The broader test results below are the earlier implementation checkpoint,
prior to removal of legacy mode. Older local group-start/strict-paired-window
fixtures describe the removed behavior and are not acceptance tests for the
current default. No full-suite pass is claimed for the cleanup.

- Both new firmware profiles build successfully.
- New coordinator, window, and inference tests cover both server variants,
  joining, timeouts, reconnects, STOP/idle behavior, model routing, and stale
  results. Run from the repository root:

  ```powershell
  python -m unittest discover -s communications/ultra96 -p 'test_dynamic*.py' -q
  ```

- The host C live-feeder test verifies an immediate join with less than a
  three-second history, next-sequence ordering, retry, and stop behavior:

  ```powershell
  gcc -std=c11 -Wall -Wextra -Werror -I communications/firmware/FireBeetle_to_Ultra96/include communications/firmware/FireBeetle_to_Ultra96/src/sensor_history.c communications/firmware/FireBeetle_to_Ultra96/one_node_tests/test_live_history.c -o .build/one_node_live_test.exe
  .build/one_node_live_test.exe
  ```

- The existing history host tests and 23 laptop relay tests pass.
- Final portable selection: 228 tests verified (226 passed in the selection,
  and two simulated-phone subprocess tests passed on rerun after exporting
  the local WebSocket dependency through `PYTHONPATH`). This includes 41 new
  dynamic-mode tests. The earlier broad-run limitations below remain separate.
- A broader selected Ultra96 run passed 221 of 223 tests. The two failures are
  C# WSS transport checks failing in Windows TLS credential acquisition. That
  run excluded the existing AI default-policy assertion: the checked-in AI
  server uses `0.04`, while its test expects `0.03` (unchanged by this work).
  Full discovery also references absent `server_new.py`. These results are not
  an all-green full-suite claim. Local logs are in `.build/one-node-*.log`.
- Physical flashing, one-/two-board acceptance tests, real inference, and
  comparative single-node/fusion accuracy remain to be verified on hardware.

See the [scenario plan](../docs/plans/SINGLE_NODE_BASELINE_AND_DYNAMIC_NODE_JOIN_PLAN.md)
for the eight agreed operating sequences.
