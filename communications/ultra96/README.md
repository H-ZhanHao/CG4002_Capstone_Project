# Ultra96 services

The current board-specific `/home/xilinx` ownership problem and temporary
deployment under `/tmp/comms` are recorded in `ULTRA96_SETUP_NOTES.md`.

`server.py` remains the ESP32 sensor service on port 5000. The independent
`visualizer_gateway.py` service sends validated inference results to one Unity
phone without allowing phone/network failures to interrupt sensor ingestion.
The active sensor topology is two direct ESP32 producers named Node 1 and
Node 2. References to Hub 3, Hub 6, or leaf forwarding in older phase records
describe the superseded assessment topology rather than current setup steps.

## Independent node startup

The new `node1_one_node` and `node2_one_node` firmware profiles support either
node starting alone and a partner joining later. Both servers now use this
behavior by default; no mode flag is needed. See
[setup, behavior, AI contract, and validation](ONE_NODE_MODE.md).

## Start the sensor server and retain logs

Stage 3 adds phone-owned rep feedback over the same WSS connection. Deploy the
updated gateway/protocol plus `rep_control.py` and `feedback_delivery.py` with
the chosen updated server (`server.py` or `server_one_node.py`). The server's
`--rep-control-port` and gateway's `--control-port` default to 9101, strictly
on Ultra96 loopback. No additional phone-facing relay port is needed.
Phone sessions must explicitly start/reset or sync before sending new reps.
Use `simulate_phone_reps.py` for verified-TLS testing, or the checked-in Unity
BeginRepSession/SendRepEvent API. See the
[Stage 3 checkpoint](../firmware/FireBeetle_to_Ultra96/test/protocol_host/STAGE3_CHECKPOINT.md)
for schemas, delivery receipts, reconnect behavior, and complete test commands.

## Integrated activity and rep outputs

In the new node profiles, each ESP LED blinks slowly while connecting and gives
three fast blinks after a valid Ultra96 HELLO_ACK. It then indicates that node's
streaming state: steadily ON after its successful START_FLUSH acknowledgement
and activity feedback, OFF while connected/armed but waiting for movement, and
OFF on AI idle pause, manual STOP or a stopped run. A connection burst finishes
before showing the streaming/idle state; connection loss resumes slow blinking.
The existing AI idle threshold and stop/ACK/rearm capture workflow are retained.
Idle is silent. Rep and ready LED flashes and the idle double beep are removed.

Phone rep sessions, sequence validation, reconnect/sync and count handling stay
in `rep_control.py`. One accepted count increase sends one 100 ms beep to Hip
(Node 1) directly over its existing TLS connection. A catch-up count generates
one confirmation beep, not a replay of missed reps. Wrist receives no rep event.
The canonical `hardware/node/BuzzerFeedback` queues rapid beeps with 100 ms gaps.
Fall alarms take priority, discard queued reps and suppress new sounds during
the alarm. Activity LED updates never silence a fall or trigger a buzzer.

Both `server.py` and `server_one_node.py` use FEEDBACK_COMMAND (0x0B), retaining its
24-byte header, 16-byte `!QB7x` payload and CONTROL_ACK format. Current actions:

| Action | Value | Destination |
| --- | --- | --- |
| REP_BUZZ | 4 | Hip only |
| ACTIVITY_ACTIVE | 5 | Both nodes |
| ACTIVITY_IDLE | 6 | Both nodes |

Old action values 1/2/3 (REP_LED/READY_LED/IDLE_BUZZ) are retired and rejected.
Deploy the updated server, `feedback_delivery.py`, `rep_control.py`, and firmware
on **both boards together**; old firmware cannot apply the new actions. No phone
JSON, gateway, TLS or fall-reporting protocol changes are required.

Feedback keeps its session validation, bounded retries and duplicate protection.
A newer activity state cancels pending retries of the older state. STOP also
clears the LED locally, and transport session begin/end resets activity and rep
sounds while preserving an active local fall alarm. Reconnecting a node stops
the shared run; LEDs remain OFF until another dual START confirmation. An ACK
means the request was handled, not that physical light or sound was measured.
Delivery remains bounded; a failed ACTIVE command can leave that board dark
until a later state/session update. An isolated board clears its LED when its
transport session ends; this is not an immediate network-loss indicator.

USB diagnostics and either server console accept `feedback rep`, `feedback
active`, `feedback idle`, and `feedback status`. Here `active`/`idle` are explicit
output tests and do not start/stop capture. Server-only `feedback repeat` resends
the last unexpired event to check duplicate suppression. USB `feedback
fall-alarm` and `fall inject` retain their separate output/event meanings.
`feedback ready` is removed. Normal operation drives activity automatically.

From the repository root, run the tracked regression checks:

```powershell
.\communications\ultra96\check_feedback_outputs.ps1
```

Physical check: boot/arm both boards (LEDs OFF), trigger and confirm both starts
(LEDs stay ON), submit AR reps (Hip beeps once each; LEDs unchanged), then confirm
AI idle (both LEDs OFF, no idle beep). Verify resume, manual STOP, reconnect,
rapid reps and fall preemption. These replace the output expectations in older
Stage 1-4 checkpoint records.

The server writes each human-readable record to both the coloured terminal and
a plain-text rotating file. Its Ultra96 defaults are:

```text
path:       /tmp/comms/logs/server.log
file size:  10 MiB
backups:    5
```

Start it with the defaults:

```sh
python server.py
```

Each connected node continuously captures locally. After HELLO, the server
synchronizes and arms each node independently without waiting for its partner.
A valid `ACTIVITY_TRIGGER` from either armed node is mapped into the Ultra96
clock and starts the ready producers. The first successful `START_FLUSH` ACK
opens the assembler at three seconds before the mapped peak. Command timeouts
isolate the affected node while another usable stream continues. A late node
synchronizes and joins live on the existing timeline without another peak.
The terminal also accepts `start` and `stop`; manual STOP persists across joins.

Stage 5 local fall reporting adds `device_events.py`, required beside either
updated server (or on its Python import path). Hip detects falls locally and
requests a three-second alarm even while capture is stopped or the network is
down. Its DEVICE_EVENT (0x13) is validated/deduplicated by the server and receives
a dedicated DEVICE_EVENT_ACK (0x14). The ESP retains up to eight pending reports
for 30 seconds, with 500 ms retries. STOP does not clear them. Receiver receipts
are bounded and retained only for the server process lifetime; neither side uses
durable storage. Logs identify synthetic events and report queue loss/expiry.
Fall AR reporting now extends this receipt path through the existing loopback
control connection (9101) and gateway WSS connection. Deploy `fall_delivery.py`
beside `device_events.py`, `rep_control.py`, `visualizer_protocol.py`, and
`feedback_delivery.py`, with the updated gateway and your selected server.
Both `server.py` and `server_one_node.py` use the shared receiver; their inference and
capture paths are unchanged. Update/rebuild the Unity client as described in
`../unity/Ultra96VisualizerDemo/README.md`. Existing firmware, laptop relay
commands, SSH forwards and TLS certificates need no changes.

The exact outbound JSON is:

```json
{"v":1,"type":"fall_event","node_id":1,"boot_id":12345,"event_id":"42"}
```

The phone replies with the same identity and `type: "fall_ack"`. The event ID
is a canonical positive decimal uint64 string; boot ID is a positive uint32
JSON integer. Other fields/types are rejected. Identity is node/boot/event,
independent of rep sessions and inference runs. `occurred_node_us`, synthetic
status and local alarm duration stay internal. Synthetic `fall inject` reports
are ACKed by Ultra96 but deliberately filtered from AR alerts.

Ultra96 retains up to 32 pending AR reports for 30 seconds from server receipt,
retries no faster than once per second per event, and replays unacknowledged
reports when the gateway/phone reconnects. At most eight are offered per 500 ms
poll to leave room for rep traffic. Full queues preserve pending reports and
drop new AR reports with `FALL_AR_OVERFLOW`; expired reports log
`FALL_AR_EXPIRED`. ESP acceptance/ACK is independent of AR availability and
does not claim AR delivery. `FALL_AR_ACKED` means the Unity client accepted the
notification, not that a human saw/dismissed it. No extra buzzer command or
automatic STOP is issued. Exercise updates never replace fall events.

This is bounded in-memory delivery: server restart loses pending reports and
receipts; phone restart loses its deduplication state. The 30-second limit
bounds server retention/retries, not the age of bytes already in transit.

Host verification (from this directory, with requirements installed):

```sh
python -m unittest -v test_fall_reporting test_stage5_fall test_rep_control
```

The fall tests exercise both server entrypoints over real loopback TLS, including
retries, reconnects, gateway restart, synthetic filtering, and concurrent rep/
exercise traffic. A physical detector-to-phone check remains required.
Hip USB `fall inject` explicitly tests the alarm/event path; `feedback fall-alarm`
still tests output only. See firmware `test/protocol_host/STAGE5_CHECKPOINT.md`
for the wire layout, saved builds, limitations and physical test procedure.

Physical capture has two stop paths. `manual_stop()` (the console `stop`)
disarms trigger handling until a manual `start`. `idle_pause()` (the console
`idle`) stops the current ESP feeders and window run, then rearms each node
after its STOP acknowledgement, so the next fresh `ACTIVITY_TRIGGER` automatically
sends `START_FLUSH`. In both cases the ESPs continue filling their local rolling
history; only network `PHYSICAL_DATA` transmission stops. An inference adapter
can call `coordinator.idle_pause()` after its idle threshold is reached. The
AI entrypoint wires the pipeline's confirmed-idle callback to the coordinator
as described below.

The AI entrypoint is `server_one_node.py`, renamed from `server_AI.py` in commit
`a0d3e59`. It imports the local `window_assembler.py` and the deployed
`inference_pipeline.InferencePipeline`. The integrated server under
`../../server/server_rep_fall.py` instead imports `window_assembler_updated.py`.
The checked-in `../../server/inference_pipeline.py` confirms
five consecutive idle classifications at confidence >= 0.80. An accepted current
generation calls `handle_idle_classification`, which pauses streaming and sends
the silent ACTIVITY_IDLE state to both nodes through the existing delivery path.
Non-idle or low-confidence results reset the streak; rejected windows do not add
evidence. Fresh capture resets evidence and generation; stale inference is
ignored. Every confirmed shared start sends ACTIVITY_ACTIVE. Manual STOP and
disconnect also clear activity; no idle buzzer event is produced. Feedback
failure does not block STOP/ACK/ARM.

For `server_one_node.py`, the default rotating log file is the absolute path
`/home/xilinx/comms/logs/server.log`, including when launched as root. Override
it with `--log-file /another/path/server.log`; rotation settings are unchanged.
The supplied pipeline is retained only as a host-test fixture in
`stage4_fixtures/inference_pipeline.py`; do not deploy that fixture over the
existing Ultra96 pipeline. See the firmware `test/protocol_host/STAGE4_CHECKPOINT.md`
for builds, archives and physical checks.

If manual START or idle needs ARM while a validated same-Session clock mapping
is older than 120 seconds but no older than ten minutes, the server uses it as
a bounded holdover and arms immediately while requesting a synchronization
refresh in the background. Missing mappings, mappings older than ten minutes,
and new Sessions still block ARM. Rejected refreshes retry after 1, 2, 4, and
then at most 8 seconds until synchronization succeeds or the operator manually
stops. `ARM_USING_MAPPING_HOLDOVER` records bounded use; `ARM_DEFERRED` records
an actual blocker and mapping ages;
`CLOCK_SYNC_REFRESH_REQUESTED` and `CLOCK_SYNC_REFRESH_RETRY` expose recovery
progress. Duplicate START and idle commands report `ARM_ALREADY_READY`,
`ARM_ALREADY_PENDING`, or `IDLE_PAUSE_ALREADY_ARMED` instead of implying that a
new command was sent.

Each authorized `START_FLUSH` advances a per-connection physical capture
generation before the command is sent. Replay and timestamp validation remain
strict within that generation, while acquisition sequences intentionally
resent by an overlapping three-second history are accepted in the new
generation. This permits a user to resume within the retained-history budget after an idle or
manual stop without the historical backfill being mistaken for a protocol
replay.

After START_FLUSH is confirmed, the server assembles provisional two-node windows
using mapped capture timestamps. The defaults are a 100 Hz expected rate, a
three-second window, and a one-second hop. This complete example makes both the
window and rotating-log settings explicit:

```sh
python server.py \
  --log-file /tmp/comms/logs/server.log \
  --log-max-mb 10 \
  --log-backups 5 \
  --sample-rate-hz 100 \
  --window-seconds 3 \
  --hop-seconds 1
```

The server options are:

| Option | Default | Meaning |
|---|---:|---|
| `--log-file` | `/tmp/comms/logs/server.log` | Active plain-text server log. |
| `--log-max-mb` | `10` | Maximum size of each log file before rotation. |
| `--log-backups` | `5` | Number of older rotated log files retained. Use `0` to retain none. |
| `--sample-rate-hz` | `100` | Expected sampling rate used for window quality checks. |
| `--window-seconds` | `3.0` | Duration of each assembled window. |
| `--hop-seconds` | `1.0` | Time between consecutive window starts. |
| `--node-wait-ms` | `250` | Capture-time grace period for a lagging partner before using available data. |

`--sample-rate-hz` changes only the expected sample count used for window
quality checks and the server's sample-interval diagnostics; it does not
change the ESP32 sampling rate. The physical firmware advertises and produces
100 Hz, matching the server default. Normal
sample intervals may vary by 10% around the configured rate before a warning
is emitted. To keep the terminal readable, one normal
sample summary is printed per node per second. Window and hop
values must be positive, and the hop cannot exceed the window. Completed or
rejected windows are summarized as `WINDOW_READY` or `WINDOW_REJECTED` without
printing their sample contents. By default, each node may be missing up to 3%
of its expected samples per window, rounded down: nine of 300 samples at
100 Hz with three-second windows. Accepted windows are padded to the expected per-node length with
inference-only samples whose six IMU values are zero; internal sequence gaps
are filled in place and edge losses are padded at the corresponding edge. The
summary reports both the received and padded counts, for example
`node1=296/300 padded=4 node2=295/300 padded=5`. If either node exceeds the
missing-sample allowance, or its sequence gaps cannot be reconciled with its count
deficit, the whole synchronized two-node window is rejected. Up to two extra
samples per node are also tolerated. Identical duplicate sequences are removed
first; unique extras are removed by an order-preserving minimum-error match
against the expected timestamp grid. The summary reports `dropped=` and
`dropped_sequences=`. More than two extras, conflicting duplicate contents, or
a sequence gap mixed with unique extras rejects the window. `server.py` remains
a window/logging adapter; `server_one_node.py` sends accepted windows to the AI
inference pipeline. It uses the available-node
assembler and inference worker; its model/runtime dependencies are supplied
separately. Its missing-sample default remains 4%, compared with 3% in
`server.py`; pass `--max-missing-fraction 0.03` to use the same limit in both.

Both servers accept `--max-missing-fraction 0.03`. Missing producers are omitted
from AI input rather than zero-filled; each included producer must satisfy the
window quality policy. There is no separate
elapsed-time gap rejection rule: a long internal or edge gap alone does not
reject an otherwise valid window. Invalid real timestamp order still reports
`reason=invalid_timestamps`. Startup `WINDOW_CONFIG` reports the effective
missing-sample count. A count deficit may reflect capture timing delays rather
than missing network packets; sequence checks remain enforced.

Padding preserves real readings and their timestamps. Synthetic edge samples
are spaced within the available edge interval, using the same time delta in
the local and coordinator clock domains, rather than repeatedly clamping to
the final microsecond. Accepted samples must be strictly increasing in both
clock domains and remain inside the half-open coordinator window. If integer
timestamp space is insufficient, the window is rejected. Synthetic IMU values
remain zero and `is_padding=True`; this change does not resample real sensor
values or establish model accuracy with padded inputs.

To deploy this policy, copy the updated `window_assembler.py` and the server
entry point actually used on Ultra96 (`server_one_node.py` for inference), then
restart that process. Keep existing model paths and launch options. The new
policy defaults apply automatically; the fraction option above can override
the missing-sample allowance. Remove any former `--max-sample-gap-ms` option
from launch commands; it is no longer supported.
No ESP firmware rebuild is needed for these server-side changes. Verify that
`WINDOW_CONFIG` shows `expected_per_node=300 max_missing_per_node=9`, inspect
rejection reasons, and compare inference
results on real exercises. The external `late_fusion_vart.py` timestamp
validator still requires an on-device integration check.

Clock synchronization uses five exchanges and builds its candidate from the
median offset of the three lowest-delay usable rounds. Periodic refreshes apply
small consistent changes immediately. A change larger than 5 ms must be
confirmed by an immediate second five-round batch that agrees within 5 ms
before the server slews toward it. Delayed, inconsistent, or unconfirmed
measurements leave the previous mapping active. The conservative defaults in
`server.py` must be reviewed during real-IMU/full-pipeline integration; the
current values and acceptance checklist are recorded in
`../firmware/FireBeetle_to_Ultra96/DEMO_TWO_NODES.md` under "Integration tuning
reminder."

After both physical nodes have an applied mapping, the coordinator emits one
neutral, non-node-coloured readiness line for that Session pair:

```text
BOTH_NODES_SYNCED nodes=[1, 2] sessions={1: ..., 2: ...} arming_activity_detection
ARM_CONFIRMED nodes=[1, 2] sequence=... arm_generation=... coordinator_arm_us=... waiting_for_activity_trigger
```

Periodic mapping refreshes do not repeat the line. A reconnect creates a new
Session pair and allows a new readiness announcement.

For example, select another writable log path and retain three 20 MiB backup
files while keeping the default window configuration:

```sh
python server.py \
  --log-file /tmp/comms/logs/custom-server.log \
  --log-max-mb 20 \
  --log-backups 3
```

`server.log` is the active file; `server.log.1` through `server.log.5` contain
successively older, non-duplicated history. Files under `/tmp` may disappear
after a reboot, so copy assessment evidence to the laptop before restarting the
board. The file intentionally contains no ANSI colour codes.

## Install the visualizer dependency

Use a virtual environment rather than installing packages globally:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
```

`requirements.txt` pins the tested WebSocket server version.

## Certificate preparation

The physical-phone path is always WSS. The certificate presented by the
Ultra96 must have an IP Subject Alternative Name for the address in Unity's
URL: `IP:192.168.0.2`. A Common Name alone is not sufficient. The laptop relay
does not terminate TLS and must never receive the private key.

For a development certificate, run this on the Ultra96 in a private,
non-repository directory:

```sh
install -d -m 700 "$HOME/.config/cg4002-visualizer"
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 30 \
  -keyout "$HOME/.config/cg4002-visualizer/server-key.pem" \
  -out "$HOME/.config/cg4002-visualizer/server-cert.pem" \
  -subj "/CN=192.168.0.2" \
  -addext "subjectAltName=IP:192.168.0.2" \
  -addext "keyUsage=digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"
chmod 600 "$HOME/.config/cg4002-visualizer/server-key.pem"
```

Do not create either file inside this repository. Transfer only
`server-cert.pem` (never the key) to the Unity development machine. The Unity
Android client must validate the SAN and either trust a project-controlled CA
or pin the expected certificate/public key. It must reject expired, mismatched,
or unexpected certificates; do not add a trust-all callback. A project CA and
CA-signed server certificate are preferable for a longer-lived setup.

## Start the WSS gateway

From the `ultra96` directory, with the virtual environment active:

```sh
python visualizer_gateway.py \
  --tls-cert "$HOME/.config/cg4002-visualizer/server-cert.pem" \
  --tls-key "$HOME/.config/cg4002-visualizer/server-key.pem" \
  --demo-source
```

The WSS listener is fixed to `127.0.0.1:9000`; the newline-delimited JSON
publisher input is fixed to `127.0.0.1:9100`. Only their port numbers are
configurable. Startup fails if either TLS file is absent or invalid. Demo mode
publishes deterministic `unknown`, `squat`, `push_up`, `sit_up`, `lunge`, and
`leg_raise` blocks at 10 Hz; the gateway rate limit defaults to 20 Hz.

A real inference process can connect locally to port 9100 and write one compact
version-1 JSON object followed by a newline. Never forward or relay port 9100.

## Tests

From this directory:

```sh
python -m unittest -v test_window_assembler.py test_server.py
python -m unittest -v test_server.py test_visualizer_protocol.py test_visualizer_gateway.py
```

Gateway tests generate a one-day loopback certificate in the operating
system's temporary directory and remove it afterwards.
