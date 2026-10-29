# Stage 4 transparent laptop relay

`tcp_relay.py` is an opaque, bidirectional TCP relay. It accepts each ESP32
node connection on laptop port `15000`, creates a matching connection to the SSH
local-forwarding listener on `127.0.0.1:15001`, and copies bytes without parsing
or modifying the sensor protocol.

## Requirements

- Python 3.9 or newer; no third-party packages are required.
- Cisco VPN and SSH access to the Ultra96 route.
- The Ultra96 `server.py` listening on `127.0.0.1:5000`.
- The existing Windows Firewall rule allowing the ESP32 to reach laptop TCP
  port `15000`.

## Start the Stage 4 path

Use separate terminals for the Ultra96 server, SSH tunnel, and relay.

1. On the Ultra96, start the existing server:

   ```bash
   python3 server.py
   ```

2. On the Windows laptop, start the SSH tunnel with a loopback-only listener:

   ```powershell
   ssh -N -o ExitOnForwardFailure=yes `
     -L 127.0.0.1:15001:127.0.0.1:5000 `
     ultra96
   ```

   Replace `ultra96` with the same SSH destination or alias used by the
   existing working tunnel. Keep this terminal open.

3. In the directory containing `tcp_relay.py`, start the relay:

   ```powershell
   py tcp_relay.py
   ```

   Its defaults are equivalent to:

   ```powershell
   py tcp_relay.py `
     --listen-host 0.0.0.0 `
     --listen-port 15000 `
     --upstream-host 127.0.0.1 `
     --upstream-port 15001
   ```

4. Reset or power on both nodes. In the validated DIR-842 setup, the laptop's
   `Wi-Fi 2` adapter is `192.168.0.2/24`, Node 1 is `192.168.0.104`, and Node 2
   is `192.168.0.132`. The addresses assigned to the nodes can change if the
   router DHCP leases change. Ethernet at `192.168.0.3/24` is retained only as
   a backup private-router connection.

Stop the relay or tunnel with `Ctrl+C`.

## Run the local tests

The tests use only loopback TCP connections and do not require the ESP32,
VPN, SSH tunnel, or Ultra96:

```powershell
py -m unittest -v test_tcp_relay.py
```

They verify byte preservation across fragmented writes, simultaneous independent
clients, and connection-pair closure when the upstream side closes.

## Stage 4 acceptance checks

With the full path running, confirm:

1. The relay logs two node connections and two independent upstream connections.
2. The Ultra96 accepts `HELLO` from Node 1 and Node 2 and allocates a different
   nonzero Session ID to each connection.
3. Node `1` and Node `2` DATA continue concurrently at 50 Hz.
4. The existing rejection and per-source HEARTBEAT tests still behave as in
   Stage 3.
5. Stop only `tcp_relay.py`. The ESP32 should enter controlled reconnect
   backoff while sampling continues and its queue stays bounded.
6. Restart `tcp_relay.py`. The ESP32 should reconnect, repeat `HELLO`, receive
   a fresh Session ID, drain its retained queue, and return to live delivery.

The relay does not provide encryption or application authentication. Stage 7
TLS will remain end-to-end between each node and the Ultra96, passing through
this relay unchanged.

## Unity-phone WSS path

The same relay can run as a second process for the visualizer. First add the
loopback-only visualizer forward to the existing SSH command:

```powershell
ssh -N -o ExitOnForwardFailure=yes `
  -L 127.0.0.1:15001:127.0.0.1:5000 `
  -L 127.0.0.1:19000:127.0.0.1:9000 `
  ultra96
```

Then start the phone-facing transparent relay from the repository root:

```powershell
py laptop/tcp_relay.py `
  --listen-host 0.0.0.0 `
  --listen-port 9000 `
  --upstream-host 127.0.0.1 `
  --upstream-port 19000
```

Unity's final private-LAN URL is:

```text
wss://192.168.0.2:9000
```

The Ultra96 certificate must contain `IP:192.168.0.2` in its Subject
Alternative Name because that is the identity Unity connects to. Unity must
validate the certificate and SAN through a trusted project CA or pin the
expected certificate/public key. Never use a trust-all callback. Keep the TLS
private key only on the Ultra96: the laptop copies the TLS handshake and WSS
frames as opaque TCP bytes and cannot decrypt them. Do not forward Ultra96
publisher port 9100 through SSH or expose it through a laptop relay.

The phone uses static address `192.168.0.200`, outside the DIR-842 DHCP pool of
`192.168.0.100` through `192.168.0.199`, and keeps **Use phone MAC** enabled.
The validated Windows Firewall inbound rule `CG4002 Visualizer Phone` permits
only that phone address to reach local address `192.168.0.2`, TCP port `9000`,
the exact Python 3.14 relay executable, and the Public profile. Windows reports
its interface type as `Any`; binding the relay and rule to local address
`192.168.0.2` selects the active `Wi-Fi 2` path. Plain `ws://` is only
acceptable for an isolated loopback development test, never for a physical
phone or final demo.

The scoped node rule `ESP32 TCP relay - DIR842` permits current Node 1 and Node 2
addresses `192.168.0.104` and `192.168.0.132` to reach local address
`192.168.0.2`, TCP port `15000`, using the exact Python 3.14 relay executable.
Recheck its remote addresses whenever the router gives either node a different
DHCP lease. The older `ESP32 TCP relay - DIR842 WiFi` fallback is also enabled;
it permits TCP port `15000` from the full `192.168.0.0/24` subnet without a
program or local-address restriction. Generic Windows `Python` rules for the
separate MSYS2 Python 3.12 executable also exist but are not the relay-specific
rules described above.

## Interactive node demo relay

Use `interactive_hub_relay.py` instead of the transparent relay when the
laptop must send keyboard-controlled test requests, benchmark commands, or
file transfers to the nodes. It terminates the node TLS connections and forwards
the existing framed binary protocol through the SSH tunnel.

```powershell
py interactive_hub_relay.py `
  --listen-host 192.168.0.2 `
  --listen-port 15000 `
  --upstream-host 127.0.0.1 `
  --upstream-port 15001 `
  --certificate "C:\path\to\demo-relay.cert.pem" `
  --private-key "C:\path\to\demo-relay.key.pem" `
  --file ..\phase7-test.bin
```

Press `1` or `2` to select Node 1 or Node 2. Press `a` to select both nodes.
The other command keys operate on the selected node or nodes.

Its automated tests run without physical nodes:

```powershell
py -m unittest -v test_interactive_hub_relay.py
```
