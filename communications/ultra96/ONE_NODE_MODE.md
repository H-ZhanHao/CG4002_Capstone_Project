# Independent startup and dynamic joining

This is the default capture
lifecycle, shared by the communications and integrated servers. No mode switch
is required. Use final `node1_final` and
`node2_final` firmware environments; implementation-stage names are historical.

## Build and choose a server

From the repository root:

```powershell
pio run -d communications/firmware/FireBeetle_to_Ultra96 -e node1_final -e node2_final
```

See [firmware configuration/flashing](../firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md)
and [Network setup](../docs/NETWORK_SETUP.md). Both final profiles use physical
100 Hz IMU capture, a 1 ms scheduler tick, TLS, live joining, serial diagnostics
and runtime Hip fall control. Building does not flash the boards.

Choose one server as described in [Ultra96 services](README.md#choose-one-sensor-server):

- `server/server_rep_fall.py` for finalized capture/window/feedback verification, together with integrated model, dashboard, rep and fall operation.
- `communications/ultra96/server.py` for capture/window/feedback verification.
- `communications/ultra96/server_one_node.py` is a retained alternative AI entry
  point requiring external model/runtime modules and separately configured paths.



Do not substitute the local reference `dynamic_inference.py` for the deployed
AI pipeline. Real-model prerequisites and missing-sample defaults are in the
server guide.

## Lifecycle and eight operating scenarios

| Situation | Capture behavior |
| --- | --- |
| Hip alone | Its own HELLO, mapping and acknowledged ARM permit a peak/start without Wrist. |
| Wrist alone | Equivalent independent capture; Hip sound/fall functions are unavailable. |
| Both ready | Either node's valid peak requests aligned starts for ready producers; ACKs are independent. |
| Partner joins active run | Synchronize and start live on the existing timeline; no new peak or global reset. |
| Partner joins before movement | Arm independently; its pending sync/ARM cannot invalidate the first node's trigger. |
| One node fails/disconnects/stalls | Isolate it and preserve another usable producer/run; recover with a fresh session/mapping. |
| All starting/streaming nodes lost | End capture; after recovery arm and wait for a fresh peak unless manually stopped. |
| Confirmed idle then movement | STOP feeders, rearm ready nodes after ACKs and start a fresh capture on movement. |

Supported, connected, synchronized, armed and acknowledged-streaming nodes are
separate states. A TCP connection alone does not make samples eligible. Session,
boot/arm identity, capture generation and command validation prevent old samples,
ACKs and inference from entering a new run. Manual STOP persists across joins.

Commands normally have a 3 s ACK deadline. A streaming producer without accepted
samples for 3 s is removed and its transport closed for normal reconnect.
Timed-out command transports are also closed; another usable producer continues.

## Live-only start and history

Final firmware accepts START_FLUSH with zero history duration as a live-only
start. It begins at the next acquisition sequence and ACKs the actual local
boundary, without inventing pre-boot samples. A late partner is not asked to
reproduce the original trigger's old history.

Initial peak startup normally requests three seconds before the mapped peak.
If history is insufficient, the server retries that producer once with a live
start. The run origin stays at the intended pre-peak boundary; early windows
without usable coverage are rejected. Another successfully started node is not
cancelled by that producer's failure. Samples remain ordered from history to live.

## Window availability and model contract

A window's `samples_by_node` contains `{1}` (Hip), `{2}` (Wrist) or `{1, 2}`
according to valid coverage. `quality.available_nodes` and rejection metadata
identify usable sources. Missing producers are omitted, not replaced by a full
zero stream. No usable source means no accepted inference window.

Known starting/lagging producers receive a bounded capture-time grace period:
when the other stream advances 250 ms beyond the window end, that window may
proceed without them. `--node-wait-ms 0` disables the grace period. This is not
an extra wall-clock sleep; emitted windows are not replayed after a partner joins.

Defaults are 100 Hz expected rate, 3 s windows and 1 s hop. Both assemblers
preserve real samples without zero padding or trimming to force the nominal
count. Capture-only defaults to a 3% missing allowance; integrated inference
defaults to 4% and resamples accepted real data downstream for model input.
Both enforce source quality and session/timestamp rules.

The AI interface is `infer_window(window)` with capture/run/window identity,
interval, exercise/class/confidence and model-window-count metadata in results.
The AI teammate has fixed single-node inference in the updated AI implementation,
as confirmed by the user on 18 November 2026. Deploy that version with this
available-node capture contract; see the [server guide](README.md#choose-one-sensor-server)
for the older local-copy synchronization requirement. Earlier capture routing
tests used deterministic outputs and do not measure the updated model's accuracy
or its comparative performance with one versus two sensors.
Missing data is not an idle result. Accepted idle confirmation uses five
consecutive confident results; stale generations cannot pause a fresh capture.

## Connection and activity feedback

Each final board slowly blinks while connecting, then emits three fast pulses
after its accepted HELLO_ACK. After the burst it is OFF while waiting and steady
ON after its local streaming/activity confirmation. Disconnect resumes slow
blinking; one partner's teardown cannot change the other's session/output state.
Idle/STOP clears activity without silencing an active local fall alarm. Hip rep
beeps are separate from LED timing. See [current outputs](README.md#integrated-activity-and-rep-outputs).

## Verification

Use the current integrated tests and portable host commands in
[Ultra96 services](README.md#verification-and-known-test-limitations) and the
[firmware host-test guide](../firmware/FireBeetle_to_Ultra96/test/protocol_host/README.md).
Older dynamic Python fixtures under `test/` still import the removed AI module
name and need maintenance before use; they are not a current all-green suite.

Physically verify each scenario, failed ACK/sync isolation, STOP persistence,
idle/resume, actual LEDs, data coverage and final model compatibility. Historical
45-test/228-check selections and their environment limitations are retained in
[Experiments and results](../docs/EXPERIMENTS_AND_RESULTS.md#7-physical-integration-and-dynamic-window-verification),
not presented here as newly rerun results.
