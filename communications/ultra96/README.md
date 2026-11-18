# Ultra96 services

This guide separates the capture-only server from the integrated inference server and the independent WSS gateway. Follow [Network setup](../docs/NETWORK_SETUP.md) for addresses,
firewall, certificates and SSH; use [deployment notes](ULTRA96_SETUP_NOTES.md)
for Python environments.

## Choose one sensor server

All sensor entry points use port 5000; run only one at a time.

| Entry point, relative to repository root | Purpose and dependencies | Window policy |
| --- | --- | --- |
| `server/server_rep_fall.py` | Finalized and integrated inference/dashboard/rep/fall server. Needs the board's VART/XIR/PYNQ runtime, NumPy/SciPy, matching models and overlay. | `server/window_assembler_updated.py`, default allowance 4%; preserves real samples without zero padding or count-forcing trimming. |
| `communications/ultra96/server.py` | Capture/window logging, terminal controls, rep and fall transport without real model execution. | Local `window_assembler.py`, default missing allowance 3%; preserves real samples without zero padding or count-forcing trimming. |
| `communications/ultra96/server_one_node.py` | Retained alternative AI entry point using the local communications assembler. Only for local integration testing purposes. | Local assembler with 4% default. It is not a self-contained substitute for the integrated deployment. |

All use independent ready-node capture/live joining by default. Final firmware
is `node1_final` / `node2_final`. Capture supports either node. 

The final demo used `--hop-seconds 0.5`; the examples below show the default one-second hop.

From a deployed repository root in the chosen environment, capture-only:

```bash
python communications/ultra96/server.py \
  --log-file /tmp/comms/logs/server.log \
  --sample-rate-hz 100 --window-seconds 3 --hop-seconds 1
```

For integrated inference, use the board's working AI environment and actual
model/overlay paths. Defaults resolve beside `server/server_rep_fall.py`:
`compiled/wrist.xmodel`, `compiled/thigh.xmodel`, `compiled/fusion.xmodel` and
`dpu_bin/dpu.bit`. Its matching `.hwh` and `.xclbin` must accompany the overlay.
A complete BOOT.BIN is not an overlay. Example from the repository root:

```bash
python server/server_rep_fall.py \
  --log-file /home/xilinx/comms/logs/server.log \
  --sample-rate-hz 100 --window-seconds 3 --hop-seconds 1 \
  --web-host 127.0.0.1
```

Use `--wrist-xmodel`, `--thigh-xmodel`, `--fusion-xmodel`, `--dpu-overlay` or
`--no-dashboard` as needed. The log directory must be writable; the example
home path is not proof the old board ownership issue has been resolved.

Keep each deployment's companion modules together. Capture-only needs
`server.py`, `window_assembler.py`, `dynamic_capture.py`, `feedback_delivery.py`,
`rep_control.py`, `device_events.py`, `fall_delivery.py`, `fall_control.py` and
`visualizer_protocol.py`. The integrated `server/` directory already contains
its matching helpers plus inference/preprocessing/runtime/dashboard modules.
Deploy a consistent revision of that directory and its real model assets.

## Start the WSS gateway

In a separate environment with `websockets==12.0` installed from
[requirements.txt](requirements.txt), run from the repository root:

```bash
python communications/ultra96/visualizer_gateway.py \
  --tls-cert "$HOME/.config/cg4002-visualizer/server-cert.pem" \
  --tls-key "$HOME/.config/cg4002-visualizer/server-key.pem"
```

Use the actual gateway certificate/key paths, not a historical temporary path.
Add `--demo-source` only for an isolated deterministic phone test, with no
competing real exercise publisher. It emits 10 Hz updates; normal output rate
limit is 20 Hz. The gateway serves one active visualizer and keeps only the
latest pending classification. Phone/gateway loss does not stop sensor ingestion.

| Port | Binding and role |
| --- | --- |
| 5000 | Sensor server binds `0.0.0.0`; SSH targets its loopback address. |
| 9000 | Gateway WSS, `127.0.0.1` only. |
| 9100 | Gateway newline-delimited exercise publisher input, loopback only. |
| 9101 | Selected sensor server's rep/fall control, loopback only. |
| 8080 | Optional integrated HTTP dashboard; default code binding is all interfaces, while the launch example above selects loopback. |

Forward WSS through the laptop's 19000/9000 route. Do not forward 9100 or 9101.
The gateway imports `visualizer_protocol.py` and `fall_delivery.py` from its own
directory; include these when deploying a minimal gateway bundle.

## Capture, windows and logging

Either independently synchronized/armed node may report an ACTIVITY_TRIGGER.
The first successful START_FLUSH ACK opens the shared run at the normal
three-second pre-peak boundary. A later ready node joins live without restarting
that run. Missing/failed/stalled nodes are isolated while another usable stream
exists. See [dynamic joining](ONE_NODE_MODE.md) for the eight operating scenarios.

| Option | Capture-only default | Integrated inference default |
| --- | --- | --- |
| `--sample-rate-hz` | 100 | 100 |
| `--window-seconds` | 3.0 | 3.0 |
| `--hop-seconds` | 1.0 | 1.0 |
| `--node-wait-ms` | 250 | 250 |
| `--max-missing-fraction` | 0.03 | 0.04 |
| `--rep-control-port` | 9101 | 9101 |
| `--log-file` | `/tmp/comms/logs/server.log` | `/home/xilinx/comms/logs/server.log` |
| `--log-max-mb`, `--log-backups` | 10 MiB, 5 backups | 10 MiB, 5 backups |

Expected rate does not change firmware acquisition. At defaults the nominal
count is 300 samples/node/window; a one-second hop is an inference opportunity,
not a guaranteed result latency. The 250 ms partner allowance is capture-time
progress, not an added wall-clock sleep. No usable source means no accepted window.

The local and integrated assemblers use the same real-sample policy: accepted
windows may contain fewer or more than the nominal count, with no synthetic
zero padding or trimming of unique real samples. Exact retransmissions are
deduplicated; at most two extras per node are tolerated, counting retransmissions
and unique extras together. The capture-only server logs these real counts;
the integrated AI pipeline resamples them onto the model's fixed input grid.
The missing-sample defaults still differ: 3% (9 of 300) for capture-only and
4% (12 of 300) for integrated inference.
Missing producers are omitted rather than fabricated as a complete sensor
stream. Review model handling of accepted gaps and missing sources on-device.
The removed `--max-sample-gap-ms` option is not supported.

Clock sync uses five rounds, three selected low-delay offsets and periodic
60 s refresh. Large corrections require a second consistent batch and bounded
slew. Same-session mapping holdover up to ten minutes can permit immediate
rearming while refresh runs; new sessions require a new mapping. Nominal 3 s
capture-control ACK and stream-liveness deadlines isolate an affected node.
These policies require physical timing/loaded-link validation.

Logs go to console and a rotating plain-text file. Copy `/tmp` logs before
reboot; the active file and numbered backups hold different segments. Do not
parse logs to implement device events or claim that configured thresholds are
measurements.

## Integrated activity and rep outputs

| Action/state | Behavior |
| --- | --- |
| Connection attempt / accepted HELLO | Slow blink / three fast confirmation pulses, owned by firmware. |
| Successful local start plus activity feedback | That node's LED steady ON. |
| Waiting, idle pause or STOP | Activity LED OFF; idle is silent. |
| Accepted phone rep count increase | One 100 ms Hip beep, including a count jump; no Wrist rep event. |
| Local Hip fall alarm | Three-second priority sound; cancels queued rep sounds and suppresses new rep sounds during the alarm. |

FEEDBACK_COMMAND `0x0B` keeps the 16-byte `!QB7x` payload and CONTROL_ACK
format. Current action values are REP_BUZZ 4 (Hip), ACTIVITY_ACTIVE 5 and
ACTIVITY_IDLE 6 (applicable connected producers). Old actions 1/2/3 are rejected.
Rep/ready LED pulses and idle double-beeps in stage checkpoints are historical.

Phone rep sessions explicitly start/reset or synchronize before new reps.
Duplicate identities/counts do not replay output. One accepted increase gives
one current confirmation, not queued missed-rep playback. Delivery ACKs mean
firmware accepted/applied/scheduled output, not measured sound/light. Rep send
success at the phone transport is distinct from final delivery results.

## Terminal commands

Type commands into the selected sensor-server terminal:

| Command | Meaning |
| --- | --- |
| `start` / `s` | Enable/retry arming ready nodes; movement still triggers capture. |
| `stop` / `x` | Stop transmission and disarm until manual start; persists across joins/reconnects. |
| `idle` / `i` | Stop feeders and rearm ready nodes after STOP ACKs for fresh movement. |
| `feedback rep`, `feedback active`, `feedback idle` | Explicit output diagnostics; do not start/stop capture. |
| `feedback status`, `feedback repeat` | Inspect delivery / retry the last unexpired feedback identity to check deduplication. |
| `fall on`, `fall off`, `fall status` | Apply/check the last acknowledged Hip detector setting. |

Acquisition/history continue during STOP and idle. The integrated AI pipeline
confirms idle after five consecutive results at confidence at least 0.80; stale
generations cannot pause a new capture. Capture-only does not autonomously run
that model-based idle decision. Feedback failure does not block STOP/ACK/rearm.

## Fall reporting and control

Final Hip detection starts ON after reboot. ON/OFF settings use binary `0x0C`
and a matching CONTROL_ACK, with one outstanding request, a 2 s deadline and no
automatic retry/replay. `fall status` is unknown after reconnect or timeout until
a setting is acknowledged; it does not query the device. No AR setting command
is supported.

Falls use DEVICE_EVENT/DEVICE_EVENT_ACK `0x13`/`0x14`, independent of capture.
The ESP retains up to eight pending reports for 30 s with 500 ms retries.
Ultra96 phone delivery retains up to 32 for 30 s with retries at least 1 s apart;
these are in-memory bounds, not durable storage. STOP does not erase a queued
fall report. Synthetic USB injection is ACKed by the server but filtered from AR.

Phone `fall_event` carries exactly version/type, node ID, boot ID and event ID;
the positive uint64 event ID is a decimal string. `fall_ack` returns the same
identity. Unity accepts/ACKs on the main thread, suppresses duplicates and exposes
a centred dismissible popup. Receipt does not establish that a human saw it.
No automatic STOP or extra buzzer command is produced by phone delivery.

## Verification and known test limitations

From the repository root, current integrated coordinator/setting checks are:

```powershell
python -B -m unittest discover -s server -p 'test_one_node_integration.py' -v
python -B -m unittest discover -s server -p 'test_server_fall_control.py' -v
```

For gateway/protocol tests on Windows, make the source directory importable:

```powershell
$env:PYTHONPATH = (Resolve-Path communications/ultra96).Path
python -B -m unittest discover -s communications/ultra96/test -p 'test_visualizer*.py' -v
```

Run in a test shell; on Linux the equivalent prefix is
`PYTHONPATH=communications/ultra96`. Gateway tests need the pinned WebSocket
package and OpenSSL. Portable firmware commands are in the
[host-test guide](../firmware/FireBeetle_to_Ultra96/test/protocol_host/README.md).

Several staged wrappers/fixtures under `test/` still reference removed
`server_AI.py`/`server_new.py` or the old directory layout. They are not a claimed
current full-suite command. Refer to [recorded validation and limits](../docs/EXPERIMENTS_AND_RESULTS.md)
for historical results. Live hardware, DPU inference, real AR callers and final
sampling/latency/accuracy measurements require their own acceptance evidence.
