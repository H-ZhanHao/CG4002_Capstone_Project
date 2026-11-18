# NUS Network Test Results

> **Historical network experiments - 19 September 2026.**
> These observations concern the tested campus/private-LAN routes and historical
> Hub 3/6 profiles, including approximately 50 Hz streaming on the working route.
> They do not identify the exact campus isolation mechanism or establish current
> firewall policy. Recorded IP addresses, removed profiles and later TLS profile
> suggestions below are historical, not instructions for the final build.
> `hub3_private_tls` and `hub6_private_tls` have also since been removed; current
> firmware uses `node1_final` and `node2_final`. See
> [current setup references](../README.md#component-guides) and
> [the consolidated route evidence](../EXPERIMENTS_AND_RESULTS.md#2-network-route-selection).

## Purpose

This document records the campus network paths that were tested before the
temporary firmware profiles were removed. It prevents the same failed paths
from being treated as untested later.

The results describe the observed network environment on 19 September 2026.
They do not claim that every NUS network or future firewall policy will behave
the same way.

## 1. ESP32 to laptop over NUS Wi-Fi

Both the FireBeetle ESP32 and the laptop could associate with NUS Wi-Fi, but an
ESP32 TCP client could not reach the laptop listener. The relay received no
connection even when the laptop listener and its local tests were healthy.

Conclusion: the tested NUS wireless path does not permit the required
ESP32-to-laptop client traffic. Wireless client isolation, VLAN separation, or
campus firewall policy is the likely boundary; the experiment did not identify
which specific network control enforced it.

Removed profiles:

- `hub3_nus_relay`
- `hub6_nus_relay`

## 2. ESP32 direct TCP to Ultra96

The ESP32 joined `NUS_STU` successfully and attempted to connect directly to
the Ultra96 research-network address on TCP port 5000. The connection did not
complete, and the Ultra96-side capture did not observe the expected incoming
connection.

Conclusion: the tested ESP32 VLAN had no usable direct TCP route to the
Ultra96 research network on port 5000. The application protocol was not the
failure point because TCP never connected.

Removed profiles and configuration:

- `hub3_nus_direct`
- `hub6_nus_direct`
- `APP_USE_DIRECT_ULTRA96_ENDPOINT`
- `tcp_endpoint_secrets.example.h`

## 3. Archived embedded-SSH feasibility work

The earlier experiment established that the ESP32:

- can join `NUS_STU`;
- can resolve `stujump.comp.nus.edu.sg`;
- can connect to the jump host on TCP port 22; and
- can receive its public `SSH-2.0-...` banner.

The laptop test of a simple jump-host forward directly to Ultra96 port 5000
was rejected with `administratively prohibited`. The normal laptop route works
by reaching Ultra96 SSH port 22 through the jump host and then forwarding from
the authenticated Ultra96 session. An ESP32 implementation would therefore
need nested SSH rather than a simple one-hop forward.

Retained association-only profiles:

- `hub3_nus_wifi` and `hub6_nus_wifi` for association-only checks;
- WPA2-Enterprise support in `wifi_manager.c`.

The TCP/22 probe and its build profile were later removed when embedded SSH was
dropped from the selected architecture.

## 4. Working campus arrangement

The successful arrangement uses an offline D-Link DIR-842 private LAN:

```text
Hub 3 --Wi-Fi--\
                 DIR-842 --Wi-Fi 2--> laptop:15000
Hub 6 --Wi-Fi--/                         |
                                          +--> SSH tunnel 127.0.0.1:15001
                                               --> Ultra96:5000
```

Validated addresses during the test:

- laptop `Wi-Fi 2`: `192.168.0.2/24`, with no gateway or DNS;
- backup laptop Ethernet: `192.168.0.3/24`;
- Hub 3: `192.168.0.104`;
- Hub 6: `192.168.0.132`;
- laptop NUS Wi-Fi remained the Internet/VPN default route.

Both hubs established independent relay and Ultra96 connections. Hub 3
received Session ID `1775741235`, Hub 6 received Session ID `348376786`, both
queues drained to zero, and both streams remained stable at approximately
50 Hz.

The original test used the now-removed `hub3_private_relay` and
`hub6_private_relay` plain-TCP profiles. Use `hub3_private_tls` and
`hub6_private_tls` for the current secure arrangement.
