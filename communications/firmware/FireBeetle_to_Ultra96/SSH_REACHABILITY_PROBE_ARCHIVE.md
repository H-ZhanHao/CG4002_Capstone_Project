# Archived FireBeetle SSH Reachability Probe

## Status

The diagnostic firmware probe was removed after the team decided not to
implement direct embedded SSH from a FireBeetle to the Ultra96. This document
preserves what was tested so the experiment is not mistaken for unfinished
production functionality.

The removed feature consisted of:

- the PlatformIO environment `hub3_nus_ssh_probe`;
- `include/ssh_reachability_probe.h`;
- `src/ssh_reachability_probe.c`; and
- the `APP_SSH_REACHABILITY_PROBE` build switch and startup path.

The generated, ignored `sdkconfig.hub3_nus_ssh_probe` snapshot was also
removed. PlatformIO will not regenerate it because the corresponding
environment no longer exists.

## What the probe did

After Hub 3 authenticated to NUS WPA2-Enterprise Wi-Fi, the one-shot task:

1. resolved `stujump.comp.nus.edu.sg`;
2. opened a TCP connection to port 22 with a five-second timeout;
3. read the public `SSH-2.0-...` identification banner; and
4. closed the socket.

It did not perform an SSH handshake, authenticate a device, verify an SSH host
key, create a tunnel, or send sensor data to the Ultra96.

## Recorded result and conclusion

The experiment proved that the tested ESP32 VLAN could resolve and reach the
NUS jump host on TCP port 22. Separate laptop testing showed that a simple
jump-host forward directly to Ultra96 port 5000 was rejected as
`administratively prohibited`. The working laptop route instead reaches the
Ultra96 SSH service through the jump host and creates the port forward from an
authenticated Ultra96 session.

An embedded equivalent would therefore require a substantially larger nested
SSH implementation, including cryptographic handshake, host-key validation,
credential storage, channel management, reconnection, resource measurement,
and two-hub concurrency. That path is outside the selected demo architecture.

## Retained related functionality

The `hub3_nus_wifi` and `hub6_nus_wifi` environments remain as isolated
enterprise-Wi-Fi association tests. They verify authentication and IP address
assignment only; they do not run SSH, ESP-NOW, mock-data streaming, or the
sensor TCP/TLS transport.

The demonstrated sensor path remains:

```text
FireBeetle hubs -> private DIR-842 LAN -> laptop relay
    -> laptop-managed SSH tunnel -> Ultra96
```
