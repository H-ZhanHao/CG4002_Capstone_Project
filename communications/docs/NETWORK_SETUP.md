# Network setup

These instructions describe the private-router deployment. The addresses and firewall names come from the
recorded working setup; verify adapter names, leases, executable paths and the
Ultra96 SSH alias on the machine being configured. This refresh did not change
any router, adapter, firewall, certificate or deployed service.

## 1. Addresses and ports

| Item | Private-LAN setting |
| --- | --- |
| Router | D-Link DIR-842, 2.4 GHz SSID; confirm its management address in the router UI. |
| Laptop private Wi-Fi adapter | Recorded name `Wi-Fi 2`; static `192.168.0.2`, prefix `/24` (mask `255.255.255.0`), no default gateway or DNS. |
| Backup laptop Ethernet | `192.168.0.3/24`; use as a replacement private connection only after adjusting endpoint/bind settings as needed. |
| Laptop campus connection | Keeps the Internet/VPN/default route used for SSH. |
| Node 1 / Hip | Recorded DHCP lease `192.168.0.104`; confirm or reserve it. |
| Node 2 / Wrist | Recorded DHCP lease `192.168.0.132`; confirm or reserve it. |
| Android phone | Static/reserved `192.168.0.200/24`, with **Use phone MAC** for this Wi-Fi. |
| Router DHCP pool | Recorded `.100` through `.199`; keep static addresses outside the pool or reserve addresses appropriately. |

On Windows, use the private adapter's IPv4 settings to assign the laptop address.
Do not apply these settings to the campus/VPN adapter. If Android requests a
gateway while entering a static address, use the private router's confirmed
address. Both wearable and phone routes must reach the laptop's `.2` address.

| Listener | Purpose |
| --- | --- |
| Laptop `192.168.0.2:15000` | Wearable TLS, terminated by `interactive_hub_relay.py`. |
| Laptop `127.0.0.1:15001` | SSH local forward to Ultra96 `127.0.0.1:5000`. |
| Ultra96 port `5000` | Chosen sensor server. Code binds `0.0.0.0`; the SSH route targets loopback. |
| Laptop `192.168.0.2:9000` | Opaque phone relay; TLS continues through to Ultra96. |
| Laptop `127.0.0.1:19000` | SSH local forward to Ultra96 `127.0.0.1:9000`. |
| Ultra96 `127.0.0.1:9000` | TLS/WSS gateway. |
| Ultra96 `127.0.0.1:9100` | Internal exercise publisher, not forwarded. |
| Ultra96 `127.0.0.1:9101` | Internal rep/fall control, not forwarded. |
| Optional dashboard | Integrated server HTTP port `8080`; use `--web-host 127.0.0.1` and a separate SSH forward if wanted. |

The recorded campus direct-TCP attempts failed. See the
[historical network experiments](results/NUS_NETWORK_TEST_RESULTS.md); they do
not establish the current campus firewall policy.

## 2. Firmware network configuration

From the repository root, configure these ignored files under
`communications/firmware/FireBeetle_to_Ultra96/include`:

| File | Values to supply |
| --- | --- |
| `wifi_secrets.h` | String macros `WIFI_SSID` and `WIFI_PASSWORD` for the private 2.4 GHz network. |
| `laptop_relay_endpoint_secrets.h` | Copy the matching `.example.h`; set `LAPTOP_RELAY_SERVER_IP` to `192.168.0.2` and port to `15000`. |
| `demo_tls_config.h` | Copy `demo_tls_config.example.h`; provide expected identity `demo-relay.local` and the actual relay CA/self-signed public certificate as correctly quoted PEM C strings. |

Use the [firmware guide](../firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md) to
build/flash `node1_final` and `node2_final`. Changing the relay IP in its private
header or changing its trusted certificate requires rebuilding the wearables.
The example certificate text is a placeholder, not a working trust anchor.

## 3. Certificates: two separate endpoints

The wearable relay and phone gateway use separate identities and keys:

| Endpoint | Required certificate identity | Private key location | Trust configuration |
| --- | --- | --- | --- |
| Wearable-facing laptop relay | DNS SAN `demo-relay.local` | Laptop, outside the repository. | Public trust material in firmware `demo_tls_config.h`. |
| Phone-facing Ultra96 WSS gateway | IP SAN `192.168.0.2` for the configured phone URL | Ultra96 only. | Exact public certificate embedded in Unity as `server-cert.bytes`. |

Reuse valid project material when available. To provision fresh self-signed
development material, run the following OpenSSL command separately on each
endpoint, from a private directory outside the checkout. Replace the paths.

Laptop relay, in PowerShell:

```powershell
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 90 `
  -keyout "C:\private\cg4002\relay-key.pem" `
  -out "C:\private\cg4002\relay-cert.pem" `
  -subj "/CN=demo-relay.local" `
  -addext "subjectAltName=DNS:demo-relay.local" `
  -addext "keyUsage=digitalSignature,keyEncipherment" `
  -addext "extendedKeyUsage=serverAuth"
```

Ultra96 gateway, in a shell with a writable home directory:

```bash
install -d -m 700 "$HOME/.config/cg4002-visualizer"
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 90 \
  -keyout "$HOME/.config/cg4002-visualizer/server-key.pem" \
  -out "$HOME/.config/cg4002-visualizer/server-cert.pem" \
  -subj "/CN=192.168.0.2" \
  -addext "subjectAltName=IP:192.168.0.2" \
  -addext "keyUsage=digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"
chmod 600 "$HOME/.config/cg4002-visualizer/server-key.pem"
```

The parent laptop directory must already exist. Keep its key accessible only to
the relay operator. Verify certificate dates and SAN with
`openssl x509 -in <certificate.pem> -noout -dates -ext subjectAltName` using the
actual path. Copy only the gateway's public certificate to the Unity machine;
see [Unity provisioning](../unity/Ultra96VisualizerDemo/README.md#supply-the-pinned-public-certificate).
Replacing that certificate requires rebuilding the phone app with the new pin.
Check device clocks when otherwise-valid certificates fail date validation.
For an unwritable Ultra96 home, use the explicitly temporary procedure in
[deployment notes](../ultra96/ULTRA96_SETUP_NOTES.md).

## 4. Windows Firewall

First select the Python environment used to run both relays. In PowerShell,
resolve its executable, then inspect the existing rules:

```powershell
$relayPython = (python -c "import sys; print(sys.executable)").Trim()
Get-NetConnectionProfile
Get-NetIPAddress -AddressFamily IPv4
Get-NetFirewallRule -DisplayName 'ESP32 TCP relay - DIR842','CG4002 Visualizer Phone' -ErrorAction SilentlyContinue |
  Format-Table DisplayName,Enabled,Profile,Direction,Action
```

If rules exist, inspect their filters rather than assuming their settings match:

```powershell
Get-NetFirewallRule -DisplayName 'ESP32 TCP relay - DIR842','CG4002 Visualizer Phone' -ErrorAction SilentlyContinue |
  Get-NetFirewallAddressFilter
Get-NetFirewallRule -DisplayName 'ESP32 TCP relay - DIR842','CG4002 Visualizer Phone' -ErrorAction SilentlyContinue |
  Get-NetFirewallPortFilter
Get-NetFirewallRule -DisplayName 'ESP32 TCP relay - DIR842','CG4002 Visualizer Phone' -ErrorAction SilentlyContinue |
  Get-NetFirewallApplicationFilter
```

The following creation template runs in **administrator PowerShell**, after
setting `$relayPython` there to the same executable and confirming node leases.
It creates only missing named rules. Existing rules are left for review; rerun
the filter inspection if an address, interpreter or network profile changes.
The recorded private-router network used the Windows **Public** profile; if
your private adapter uses another profile, use that profile consistently.

```powershell
$relayPython = (python -c "import sys; print(sys.executable)").Trim()
if (-not (Get-NetFirewallRule -DisplayName 'ESP32 TCP relay - DIR842' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName 'ESP32 TCP relay - DIR842' `
      -Direction Inbound -Action Allow -Enabled True -Profile Public `
      -Protocol TCP -LocalAddress 192.168.0.2 -LocalPort 15000 `
      -RemoteAddress 192.168.0.104,192.168.0.132 -Program $relayPython
}
if (-not (Get-NetFirewallRule -DisplayName 'CG4002 Visualizer Phone' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName 'CG4002 Visualizer Phone' `
      -Direction Inbound -Action Allow -Enabled True -Profile Public `
      -Protocol TCP -LocalAddress 192.168.0.2 -LocalPort 9000 `
      -RemoteAddress 192.168.0.200 -Program $relayPython
}
```

Run the relays with this exact Python executable. A narrowly scoped new rule
does not override an existing broader allow rule. Historical notes describe an
`ESP32 TCP relay - DIR842 WiFi` subnet-wide fallback and generic Python rules;
inspect those separately before claiming the effective policy is restricted
to these three devices. Their present state was not audited during this refresh.

## 5. SSH tunnel and Programs to Startup

Connect the laptop's campus/VPN route and verify the existing `ultra96` SSH
alias reaches the correct board through the required jump host. The alias is
machine-specific and is not provisioned by this repository. Do not put account
credentials into project documentation.

From a laptop terminal, keep this tunnel running:

```powershell
ssh -N -o ExitOnForwardFailure=yes `
  -L 127.0.0.1:15001:127.0.0.1:5000 `
  -L 127.0.0.1:19000:127.0.0.1:9000 `
  ultra96
```

Replace only the SSH alias if your configured destination differs. Ports 9100
and 9101 stay inside Ultra96. For an optional loopback-bound dashboard, add
`-L 127.0.0.1:18080:127.0.0.1:8080` and browse laptop `http://127.0.0.1:18080`.

For manual startup, use separate terminals for the following services. 

For our final demo, we configured the Ultra96 server and WSS gateway as systemd services to start automatically at boot. Skip steps 1–2 if these services are already running.

1. On Ultra96, start one [Ultra96 sensor server](../ultra96/README.md#choose-one-sensor-server). For final demo, we used `server/server_rep_fall.py` with `--hop-seconds 0.5`. This server uses [window_assembler_updated.py](../../server/window_assembler_updated.py) to assemble timestamped IMU samples into sliding windows.
2. On Ultra96, start the [WSS gateway](../ultra96/README.md#start-the-wss-gateway) with the real gateway certificate and key. Use `--demo-source` only for isolated phone testing, not alongside real results.
3. Start the SSH tunnel above.
4. Start the TLS wearable relay and transparent phone relay using the
   [laptop commands](../laptop/README.md#start-the-two-relays).
5. Power the configured wearables and launch the provisioned phone app provided by Visualizer teammate.


## 6. Verify each hop

On the laptop:

```powershell
Get-NetTCPConnection -State Listen -LocalPort 15000,15001,9000,19000
Test-NetConnection 127.0.0.1 -Port 15001
Test-NetConnection 127.0.0.1 -Port 19000
```

On Ultra96, inspect `ss -ltn` for the chosen server and gateway listeners.
An open TCP port is a reachability check, not proof of certificate validation
or an applied command. Confirm firmware TLS/HELLO acceptance, independent
node IDs/sessions and server synchronization/ARM logs. Move a sensor, then
check accepted START acknowledgements, history/live samples and window logs.
Compare phone run/sequence with gateway logs; test stale/disconnect/reconnect.

| Symptom | Check |
| --- | --- |
| No wearable connection | Private SSID, DHCP leases, `.2` adapter address, port 15000 listener, endpoint header and the effective firewall rules. |
| TLS handshake rejected | Correct endpoint's certificate/key, trusted public certificate, SAN/expected identity, expiry and clock. |
| Relay has no upstream | VPN/SSH route, forward ports, actual server port and accidental second server. |
| Phone disconnected | Phone `.200`, Use phone MAC, port 9000 firewall, transparent relay and gateway pin. |
| Windows report bad windows | Measured sample cadence, chosen assembler/policy and source presence; see sampling diagnostics. |
| Old profile/module not found | Use final profiles and the current server module layout; historical stage names are not current setup commands. |

No network settings or deployment checks are considered verified just because
these templates exist. Record actual device evidence using
[Experiments and results](EXPERIMENTS_AND_RESULTS.md).
