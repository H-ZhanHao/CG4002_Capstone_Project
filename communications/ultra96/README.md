# Ultra96 services

The current board-specific `/home/xilinx` ownership problem and temporary
deployment under `/tmp/comms` are recorded in `ULTRA96_SETUP_NOTES.md`.

`server.py` remains the ESP32 sensor service on port 5000. The independent
`visualizer_gateway.py` service sends validated inference results to one Unity
phone without allowing phone/network failures to interrupt sensor ingestion.
The active sensor topology is two direct ESP32 producers named Node 1 and
Node 2. References to Hub 3, Hub 6, or leaf forwarding in older phase records
describe the superseded assessment topology rather than current setup steps.

## Start the sensor server and retain logs

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

The physical path continuously captures on both nodes. After HELLO, the server
synchronizes both clocks; only then does it accept an `ACTIVITY_TRIGGER` from
either node. It maps the reported peak into the Ultra96 clock and sends one
`START_FLUSH` to each node. Both commands must receive `CONTROL_ACK` within
three seconds. The server then opens its assembler at five seconds before the
common peak. A timeout sends STOP to both and requires a manual `start` to
re-arm trigger handling. The terminal also accepts `start` and `stop`.

After START_FLUSH is confirmed, the server assembles provisional two-node windows
using mapped capture timestamps. The defaults are a 50 Hz expected rate, a
five-second window, and a one-second hop. This complete example makes both the
window and rotating-log settings explicit:

```sh
python server.py \
  --log-file /tmp/comms/logs/server.log \
  --log-max-mb 10 \
  --log-backups 5 \
  --sample-rate-hz 50 \
  --window-seconds 5 \
  --hop-seconds 1
```

The server options are:

| Option | Default | Meaning |
|---|---:|---|
| `--log-file` | `/tmp/comms/logs/server.log` | Active plain-text server log. |
| `--log-max-mb` | `10` | Maximum size of each log file before rotation. |
| `--log-backups` | `5` | Number of older rotated log files retained. Use `0` to retain none. |
| `--sample-rate-hz` | `50` | Expected sampling rate used for window quality checks. |
| `--window-seconds` | `5.0` | Duration of each assembled window. |
| `--hop-seconds` | `1.0` | Time between consecutive window starts. |

`--sample-rate-hz` changes only the expected sample count used for window
quality checks and the server's sample-interval diagnostics; it does not
change the ESP32 sampling rate. The physical firmware currently advertises and
produces 50 Hz. Normal sample intervals may vary by 10% around the configured
rate before a warning is emitted. To keep the terminal readable, one normal
sample summary is printed per node per second. Window and hop
values must be positive, and the hop cannot exceed the window. Completed or
rejected windows are summarized as `WINDOW_READY` or `WINDOW_REJECTED` without
printing their sample contents. This is a neutral adapter for later AI
integration and does not run inference, resample, fill, or normalize data.

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
