# CG4002 Communications

Communication subsystem for the CG4002 wearable exercise project.

## Current components

- `firmware/FireBeetle_to_Ultra96`: PlatformIO firmware for communication between a FireBeetle ESP32 and the Ultra96 system.
- `laptop/tcp_relay.py`: transparent multi-node relay into the supervised SSH tunnel.
- `laptop/interactive_hub_relay.py`: TLS-terminating interactive relay for
  requests, benchmarks, file transfers, and per-node stream statistics.
- `ultra96/server.py`: concurrent protocol server for Node 1 and Node 2.
- `ultra96/visualizer_gateway.py`: loopback-only WSS gateway for one Unity-phone visualizer.
- `ultra96/visualizer_protocol.py`: version-1 exercise-update validation and serialization.

## Current working communication path

Node 1 + Node 2 → private DIR-842 LAN → laptop relay → SSH tunnel → Ultra96

The direct NUS-Wi-Fi TCP experiments were unsuccessful and their temporary
build profiles have been removed. See the
[NUS network test results](docs/results/NUS_NETWORK_TEST_RESULTS.md) for the
evidence and conclusions.

The earlier FireBeetle TCP/22 reachability probe was removed after embedded
SSH was dropped from the selected architecture. Its result and limitations are
preserved in `firmware/FireBeetle_to_Ultra96/SSH_REACHABILITY_PROBE_ARCHIVE.md`.
The NUS WPA2-Enterprise association-only profiles remain available.

## Development status

The ESP32-to-Ultra96 sensor path remains on port 5000. The separate encrypted
Ultra96-to-Unity visualizer path is documented in `ultra96/README.md` and
`laptop/README.md`. Current demonstrated results and remaining acceptance work
are tracked in the
[visualizer progress report](docs/progress/VISUALIZER_PROGRESS.md).

## Documentation

- [Documentation index](docs/README.md)
- [Laptop relay guide](laptop/README.md)
- [Ultra96 services guide](ultra96/README.md)
- [Unity visualizer guide](unity/Ultra96VisualizerDemo/README.md)
- [Two-node firmware demo guide](firmware/FireBeetle_to_Ultra96/DEMO_TWO_NODES.md)
