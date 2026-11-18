# Documentation

The repository keeps instructions beside the components they describe. Longer
project-wide records are grouped here by purpose.

Maintained guides were reviewed against the repository on 16 November 2026.
Source-checked defaults and documented test procedures do not establish new
physical acceptance or a current all-green regression suite.

## Setup entry point

Start with [Network setup](NETWORK_SETUP.md) for router/IP configuration,
Windows Firewall inspection/creation, both TLS identities, SSH forwarding,
startup order and per-hop checks. Use the component guides for firmware,
server selection, tests and Unity integration.

## Current topology

The active implementation uses two independent direct producers, Node 1 (Thigh/Hip)
and Node 2 (Wrist), with independent startup and live joining. Current firmware
profiles are `node1_final` and `node2_final`; older profile names in historical
records are not current build commands. See the firmware configuration and
component guides below. Documents that mention Hub 3, Hub 6, leaves,
or ESP-NOW are retained as historical phase evidence; their identifiers and
topology must not be applied to the current build.

## Design explanations

- [Design evolution, original milestones and user workflow](DESIGN_EVOLUTION.md)
- [Communications design choices and tradeoffs](COMMUNICATIONS_DESIGN_CHOICES.md)

## Experiments and results

- [Experiments, recorded measurements, validation limits and pending checks](EXPERIMENTS_AND_RESULTS.md)
- [Detailed NUS network test results](results/NUS_NETWORK_TEST_RESULTS.md)

The two consolidated records replace seven implementation plans, the old
presentation walkthrough and the September visualizer progress note. Their
[source map](DESIGN_EVOLUTION.md#source-consolidation) identifies what was
retained. Historical handoffs and checkpoints remain supporting evidence;
their rates, topology, output patterns and test counts describe their own revisions.

## Implementation handoffs

The Phase 1-10 handoffs record preparation for the Individual Subsystem Test
on 1 October. The hardware-communications vertical-slice handoff covers
subsequent integration work.

- [Hardware–communications vertical-slice implementation](handoffs/HARDWARE_COMMUNICATIONS_VERTICAL_SLICE.md)
- [Phases 1–6 physical test results and handoff](handoffs/PHASE_1_TO_6_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 7 physical test results and handoff](handoffs/PHASE_7_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 8 physical test results and handoff](handoffs/PHASE_8_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 9 physical test results and handoff](handoffs/PHASE_9_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 10 implementation handoff](handoffs/PHASE_10_IMPLEMENTATION_HANDOFF.md)

## Component guides

- [Final firmware profiles](../firmware/FireBeetle_to_Ultra96/platformio.ini)
- [Laptop relays](../laptop/README.md)
- [Ultra96 services](../ultra96/README.md)
- [Independent startup and dynamic joining](../ultra96/ONE_NODE_MODE.md)
- [Ultra96 deployment notes](../ultra96/ULTRA96_SETUP_NOTES.md)
- [Hip and Wrist firmware guide](../firmware/FireBeetle_to_Ultra96/FIRMWARE_GUIDE.md)
- [Sampling diagnostics](../firmware/FireBeetle_to_Ultra96/SAMPLING_DIAGNOSTICS.md)
- [Hardware integration boundary](../firmware/FireBeetle_to_Ultra96/components/hardware_core/README.md)
- [Portable firmware tests and historical-runner limits](../firmware/FireBeetle_to_Ultra96/test/protocol_host/README.md)
- [Unity visualizer demo](../unity/Ultra96VisualizerDemo/README.md)
