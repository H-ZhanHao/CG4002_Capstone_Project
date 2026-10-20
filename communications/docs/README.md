# Documentation

The repository keeps instructions beside the components they describe. Longer
project-wide records are grouped here by purpose.

- [AI and Unity team integration brief](TEAM_INTEGRATION_BRIEF.md)

## Current topology

The active implementation uses two independent direct producers, Node 1 and
Node 2. Use the two-node firmware, laptop, and Ultra96 component guides below
for current setup instructions. Documents that mention Hub 3, Hub 6, leaves,
or ESP-NOW are retained as historical phase evidence; their identifiers and
topology must not be applied to the current build.

## Plans

- [Hardware–communications integration plan](plans/HARDWARE_COMMUNICATIONS_INTEGRATION_PLAN.md)
- [ESP32 individual demo implementation plan (historical)](plans/ESP32_INDIVIDUAL_DEMO_IMPLEMENTATION_PLAN.md)
- [Ultra96 sliding-window MVP implementation plan](plans/ULTRA96_SLIDING_WINDOW_MVP_IMPLEMENTATION_PLAN.md)
- [Continuous IMU streaming and sliding-window proposal](plans/CONTINUOUS_IMU_STREAMING_AND_SLIDING_WINDOW_PLAN.md)
- [Ultra96-to-Unity visualizer implementation plan](plans/ULTRA96_TO_UNITY_VISUALIZER_IMPLEMENTATION_PLAN.md)

## Implementation handoffs

- [Hardware–communications vertical-slice implementation](handoffs/HARDWARE_COMMUNICATIONS_VERTICAL_SLICE.md)
- [Phases 1–6 physical test results and handoff](handoffs/PHASE_1_TO_6_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 7 physical test results and handoff](handoffs/PHASE_7_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 8 physical test results and handoff](handoffs/PHASE_8_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 9 physical test results and handoff](handoffs/PHASE_9_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
- [Phase 10 implementation handoff](handoffs/PHASE_10_IMPLEMENTATION_HANDOFF.md)

## Results and progress

- [NUS network test results](results/NUS_NETWORK_TEST_RESULTS.md)
- [Ultra96-to-phone visualizer progress](progress/VISUALIZER_PROGRESS.md)

## Walkthrough material

- [Physical live demo one-day guide](walkthrough/LIVE_DEMO_ONE_DAY_GUIDE.md)
- [Protocol FSM diagrams](walkthrough/PROTOCOL_FSM_DIAGRAMS.md)
- [Laptop and Ultra96 concurrency block diagram](walkthrough/LAPTOP_ULTRA96_CONCURRENCY.md)
- [Live demo protocol explanation](walkthrough/LIVE_DEMO_PROTOCOL_EXPLANATION.md)
- [FireBeetle protocol and concurrent connections](walkthrough/FIREBEETLE_PROTOCOL_AND_CONCURRENT_CONNECTIONS.md)
- [Maximum-speed benchmark and file-transfer demo](walkthrough/BENCHMARK_AND_FILE_TRANSFER_DEMO.md)

## Component guides

- [Laptop relays](../laptop/README.md)
- [Ultra96 services](../ultra96/README.md)
- [Ultra96 deployment notes](../ultra96/ULTRA96_SETUP_NOTES.md)
- [Two-node firmware demo](../firmware/FireBeetle_to_Ultra96/DEMO_TWO_NODES.md)
- [Unity visualizer demo](../unity/Ultra96VisualizerDemo/README.md)
