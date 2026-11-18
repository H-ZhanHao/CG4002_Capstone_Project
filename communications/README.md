# CG4002 communications


The subsystem connects independent Thigh (as known as Hip) and Wrist wearables to Ultra96, publishes
exercise results to Unity, and carries phone rep feedback and Thigh fall events.

## Start here

1. Follow [Network setup](docs/NETWORK_SETUP.md) for private-LAN IPs, firewall
   rules, the two TLS identities and SSH forwarding.
2. Configure/build the final wearables using the
   [firmware guide](firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md).
3. Choose one [Ultra96 server](ultra96/README.md#choose-one-sensor-server),
   start its WSS gateway and start the [laptop relays](laptop/README.md).
4. Set up the final phone visualizer app provided by our Visualizer teammate,
   including provisioning the gateway public certificate. The local
   [Unity directory](unity/Ultra96VisualizerDemo/README.md) contains our
   communications components and integration instructions.
5. Verify the full path and retain the measured results. Documentation/source
   settings alone do not establish physical acceptance.

## Current components and routes

| Component | Role |
| --- | --- |
| `firmware/FireBeetle_to_Ultra96` | Final PlatformIO environments `node1_final` (Thigh/Hip) and `node2_final` (Wrist), plus four sampling diagnostic environments. |
| `laptop/interactive_hub_relay.py` | Terminates wearable TLS on private-LAN port 15000 and forwards binary frames into SSH. |
| `laptop/tcp_relay.py` | Opaque phone relay on private-LAN port 9000; phone TLS terminates on Ultra96. |
| `ultra96/server.py` | Capture/window/feedback verification without real model execution. |
| `../server/server_rep_fall.py` | Integrated model/dashboard/rep/fall server using the real-sample assembler. |
| `ultra96/visualizer_gateway.py` | Loopback WSS gateway for one active phone; local publisher/control ports 9100/9101. |

Each wearable joins the private DIR-842 LAN and connects independently to the
laptop TLS relay, then through SSH to Ultra96 port 5000. Phone WSS passes through
the laptop's separate transparent relay and SSH forward to Ultra96 port 9000.
The project does not create or supervise the SSH tunnel automatically.

Final configuration is 100 Hz acquisition, seven seconds of local history,
three-second inference windows and a one-second hop. Either synchronized node
can start capture; a partner can join live. A generic peak starts history/live
transmission; STOP/idle leave local sampling running. Hip provides rep beeps
and local fall alarm/reporting; idle is silent. Final fall detection starts ON
and is controlled from the server terminal.

Single-node inference is supported by the AI teammate's updated implementation.
Use the matching updated AI runtime; the [server guide](ultra96/README.md#choose-one-sensor-server)
records the local-copy synchronization needed for this checkout. Window policy
also differs between the communications and integrated servers.

## Documentation and evidence

- [Documentation index](docs/README.md)
- [Network setup](docs/NETWORK_SETUP.md)
- [Design evolution and user workflow](docs/DESIGN_EVOLUTION.md)
- [Communications design choices](docs/COMMUNICATIONS_DESIGN_CHOICES.md)
- [Experiments, evidence limits and pending measurements](docs/EXPERIMENTS_AND_RESULTS.md)
- [Sampling diagnostics](firmware/FireBeetle_to_Ultra96/SAMPLING_DIAGNOSTICS.md)
- [Portable firmware checks](firmware/FireBeetle_to_Ultra96/test/protocol_host/README.md)

Historical Hub 3/6, leaf/ESP-NOW, campus association and stage-specific firmware
profiles are not current build environments. The retained historical records
explain that work; final commands use the checked-in `platformio.ini`.
