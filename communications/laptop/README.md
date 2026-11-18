# Laptop relays

Use the interactive TLS
relay for wearables and the transparent relay for the phone. IP assignment,
firewall creation/inspection, certificate provisioning and SSH commands are in
[Network setup](../docs/NETWORK_SETUP.md).

## Requirements

- A Python environment supporting the relay source's type annotations (Python
  3.10 or newer is suitable); these two relays use the standard library.
- Private-router connectivity, the selected TLS relay certificate/key, and a
  working VPN/SSH route to Ultra96.
- One chosen Ultra96 sensor server on port 5000 and the WSS gateway for the phone.
- Firewall rules matching the actual Python executable and current device IPs.

## Start the two relays

Run commands from the **repository root** on Windows. Start the SSH tunnel
from [Network setup](../docs/NETWORK_SETUP.md#5-ssh-tunnel-and-startup-order)
first and keep it running in a separate terminal. The tunnel listens on laptop
loopback ports 15001 (sensor) and 19000 (phone).

Wearable relay, in its own PowerShell terminal:

```powershell
python communications/laptop/interactive_hub_relay.py `
  --listen-host 192.168.0.2 --listen-port 15000 `
  --upstream-host 127.0.0.1 --upstream-port 15001 `
  --certificate "C:\private\cg4002\relay-cert.pem" `
  --private-key "C:\private\cg4002\relay-key.pem"
```

Use the paths to the actual relay certificate and key. Its DNS SAN must match
the firmware's expected identity, normally `demo-relay.local`; the firmware must
trust its public certificate/CA. The relay decrypts wearable messages and sends
the binary protocol onward inside SSH. Starting without certificate/key does
not provide the final wearable TLS setup.

Phone relay, in a second PowerShell terminal:

```powershell
python communications/laptop/tcp_relay.py `
  --listen-host 192.168.0.2 --listen-port 9000 `
  --upstream-host 127.0.0.1 --upstream-port 19000
```

This copies the phone's TLS handshake and WSS frames unchanged. The gateway key
stays on Ultra96. The phone connects to `wss://192.168.0.2:9000`; its embedded
public certificate must match the Ultra96 gateway, not the laptop relay.

Each incoming connection gets its own upstream connection. Do not run both
relay programs on port 15000: the raw transparent relay cannot terminate the
final firmware's TLS for the plaintext Ultra96 sensor server. Its generic
port-15000 defaults are not the selected sensor startup procedure.

## Diagnostics and file transfer

In the interactive relay, `1`/`2` select a node and `a` selects both. Keyboard
requests, duplicate/conflict checks, benchmarks and file operations are
maintenance tools; physical capture still depends on Ultra96's ARM/trigger/
START_FLUSH lifecycle. Quit with `q` or Ctrl+C. Per-node output identifies
sessions and stream statistics.

To enable file-transfer diagnostics, append `--file "C:\path\to\test.bin"` to
its launch command. File keys include `f` for a valid transfer, `r` for corrupted
CRC, `o` for wrong offset and `t` for inactivity timeout. The ESP validates and
forwards bounded chunks; benchmark/file figures in the historical handoffs do
not prove the same performance on final physical firmware.

## Local tests

From the repository root, in a test shell:

```powershell
$env:PYTHONPATH = (Resolve-Path communications/laptop).Path
python -B -m unittest discover -s communications/laptop/test -p 'test_*.py' -v
```

On Linux, use
`PYTHONPATH=communications/laptop python -B -m unittest discover -s communications/laptop/test -p 'test_*.py' -v`.

These exercise relay/request behavior over local connections without the
boards, campus route or physical phone. They do not validate the actual Windows
firewall, deployed certificate provisioning or radio performance.

## End-to-end checks

1. Confirm private listeners on 15000/9000 and SSH listeners on 15001/19000.
2. Power one wearable; expect TLS, a nonzero HELLO session and independent sync/
   ARM. Move it to trigger history/live delivery. Repeat with the other board.
3. Connect the phone and compare run/sequence with the gateway log. Stop/restart
   only its relay to check freshness and reconnection while sensor ingestion continues.
4. Reset one wearable; the remaining usable capture stream should continue.
   The returning board gets a fresh session/mapping and joins the active timeline.
5. Check sampling/window diagnostics under normal load; the final source is
   configured for 100 Hz, which requires physical measurement to substantiate.

For IP, TLS, firewall and upstream failures use the
[network troubleshooting table](../docs/NETWORK_SETUP.md#6-verify-each-hop).
