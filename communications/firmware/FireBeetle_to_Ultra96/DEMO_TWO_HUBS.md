# Archived Two-Hub Ultra96 Demo

This page describes the earlier Hub 3/Hub 6 and optional Leaf 1 configuration.
It is retained as historical evidence only. The active design has two direct
ESP32 producers named Node 1 and Node 2, with no leaf-hub forwarding.

Use [DEMO_TWO_NODES.md](DEMO_TWO_NODES.md) for the current build and demo
procedure. Commands and identifiers below are not valid for the current
firmware profiles.

## One-time configuration

1. Put the DIR-842 2.4 GHz SSID and password only in the ignored
   `include/wifi_secrets.h`.
2. Set `LAPTOP_RELAY_SERVER_IP` in the ignored
   `include/laptop_relay_endpoint_secrets.h` to the laptop's static `Wi-Fi 2`
   address, `192.168.0.2`. Keep its port at `15000`.
3. Put the updated `ultra96/server.py` on the Ultra96. It must be this
   concurrent version; the earlier server processed only one client at a time.
4. Give the laptop `Wi-Fi 2` adapter `192.168.0.2/24` with no gateway or DNS.
   Keep the other Wi-Fi connection as the laptop's NUS Internet/VPN route.
   Ethernet at `192.168.0.3/24` is a backup private-router connection, not the
   active demo path.

## Demo procedure

On the Ultra96, start packet capture first, then start the server:

```sh
sudo tcpdump -ni any 'tcp port 5000'
python3 server.py
```

Flash physical Hub 1 on COM7 as protocol Hub 3:

```powershell
pio run -e hub3_private_tls -t upload --upload-port COM7
```

Flash physical Hub 2 on COM9 as protocol Hub 6:

```powershell
pio run -e hub6_private_tls -t upload --upload-port COM9
```

Start the Ultra96 server, the loopback-only SSH forward, and
`laptop/interactive_hub_relay.py`, then power both boards. The Ultra96 should report two
accepted HELLO messages: `hub=3` with assigned mask `0x0E`, and `hub=6` with
assigned mask `0x70`. Node 3 and Node 6 DATA should arrive concurrently with a
different Session ID for each connection.

## Archived embedded-SSH experiment

The former FireBeetle TCP/22 reachability probe was removed because embedded
SSH is no longer part of the selected architecture. Its purpose, observed
result, and limitations remain documented in
`SSH_REACHABILITY_PROBE_ARCHIVE.md`.

## Private-router laptop relay (recommended on campus)

The DIR-842 provides an offline wireless private LAN for the two hubs, phone,
and laptop. The laptop remains connected to NUS Wi-Fi for VPN/SSH while its
`Wi-Fi 2` adapter joins the DIR-842 with static address `192.168.0.2/24` and no
gateway or DNS. The Ethernet adapter's backup address is `192.168.0.3/24`.

These profiles create two independent local mock streams and disable ESP-NOW:

```text
Hub 3 --Wi-Fi--\
                 DIR-842 --Wi-Fi 2--> laptop:15000
Hub 6 --Wi-Fi--/                         |
                                          +--> SSH tunnel 127.0.0.1:15001
                                               --> Ultra96:5000
```

The DIR-842 WAN/Internet port is not used. The validated Windows Firewall hub
rule allows only the current Hub 3 and Hub 6 addresses to reach the exact
Python relay executable on laptop address `192.168.0.2`, TCP port `15000`, via
the local address used by `Wi-Fi 2`. Windows currently reports the rule's
interface type as `Any`; the `192.168.0.2` local-address restriction is what
selects the active private-router path. Update the remote addresses if the
router assigns different hub addresses. An older, broader
`ESP32 TCP relay - DIR842 WiFi` rule also remains enabled for TCP port `15000`
from the entire `192.168.0.0/24` subnet; it is a fallback, not the narrowly
scoped rule used to describe the final setup. The relay creates a separate
upstream connection for every hub, and the Ultra96 server handles them
concurrently.

The discarded direct-NUS and NUS-laptop-relay experiments are recorded in the
[NUS network test results](../../docs/results/NUS_NETWORK_TEST_RESULTS.md);
their temporary firmware profiles are no longer present.

## Restore the second board as Leaf 1

```powershell
pio run -e leaf1 -t upload --upload-port COM9
```

Restoring `leaf1` re-enables the existing Leaf 1 to Hub 3 ESP-NOW behavior.
Before using it on campus, make sure its configured ESP-NOW channel matches
Hub 3's active WLAN channel.
