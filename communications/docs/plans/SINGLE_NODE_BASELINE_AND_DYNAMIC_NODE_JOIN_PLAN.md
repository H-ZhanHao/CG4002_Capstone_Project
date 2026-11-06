# Single-node baseline and dynamic node joining

**Status:** Default server behavior implemented; physical and real-model validation pending  
**Date:** 2026-11-06

## Summary of the current work

Implementation update (2026-11-06): the agreed sequences are now the default
behavior of both servers, using firmware profiles `node1_one_node` and
`node2_one_node`. The legacy wait-for-both coordinator and `--one-node` switch
have been removed. Existing PlatformIO profiles were preserved. The AI input
contract is assumed, per user direction, to accept windows with either node or
both through `infer_window(window)`. See the
[implementation guide](../../ultra96/ONE_NODE_MODE.md) for commands, tests,
timeouts, and remaining hardware validation. The original findings and proposed
sequences below record the pre-implementation design context.

We are documenting how to move from a coordinated two-node requirement to a
system that operates with either node independently and incorporates the other
node when it becomes available. We have inspected the generic peak detector,
firmware capture bridge, Ultra96 coordination, and window assembly paths.
The original documentation step recorded the intended sequences and scope
before changing runtime behavior. The implementation update above describes
the subsequent code changes and points to their validation record.

The objective is a working single-node baseline, with two-node input enabling
fusion inference. Improved accuracy from two nodes is an intended benefit to
validate, rather than a guarantee of this architecture.

## Intended behavior

- Either node can be the only connected node at startup.
- A ready node can detect activity, stream samples, and supply single-node AI
  inference without waiting for its partner.
- A second node can join an active capture without restarting the first node,
  requiring another peak, or resetting the capture timeline.
- AI uses both nodes when the same window has sufficient valid data from both.
- Loss of one node does not stop a usable stream from the other.
- Node availability determines the available AI inputs; activity determines
  when a capture starts.

Node 1 is the hip/waist role and node 2 is the wrist role. Single-node operation
does not imply that hardware-specific features exist on either board: wrist-only
operation cannot provide the hip board's local buzzer or fall monitoring.

## Current code findings

| Area | Verified current behavior | Implication |
|---|---|---|
| Generic peak detection | Communications builds copied `GenericPeakDetector.cpp` and `.h` files. Compared with `hardware/node`, each differs only by an extra trailing blank line. | Keep the existing detector algorithm for this work. |
| Detector input | Each firmware instance passes its own IMU samples to its local detector through `hardware_bridge.cpp`. | Either node can detect movement independently. |
| Capture coordination | Both Ultra96 server variants default to required nodes `(1, 2)` and gate arming/start on that set. | Single-node startup needs coordinator changes. |
| Connection changes | Registering or unregistering a node stops an armed or active run. | Joining and individual node loss need separate handling. |
| Clock mapping | Each node independently estimates its offset against Ultra96. | The existing synchronization algorithm can support different boot/join times. |
| Window assembly | The checked-in assembler waits for every configured node to reach the window end. | An absent or stalled node can block window production. |
| Firmware `START_FLUSH` | Requires nonzero history duration and sufficient retained history. | Reusing the original peak command for a late node is unsafe; define a join operation. |
| AI integration | `server_AI.py` loads wrist, thigh, and fusion models through external dependencies. | Independent classification support still needs verification with the AI implementation. |

The generic detector uses three-axis acceleration magnitude deviation from
gravity and gyro magnitude, a three-sample median filter, gates of 0.50 m/s² or
0.60 rad/s, 100 ms activity qualification, a 25% score decline confirmed for
40 ms, quiet-time rearming, and a one-second trigger cooldown. It is a movement
trigger, not an exercise classifier or repetition counter.

Relevant sources:

- [Communications hardware component build](../../firmware/FireBeetle_to_Ultra96/components/hardware_core/CMakeLists.txt)
- [Communications generic detector](../../firmware/FireBeetle_to_Ultra96/components/hardware_core/src/GenericPeakDetector.cpp)
- [Detector configuration](../../firmware/FireBeetle_to_Ultra96/components/hardware_core/include/GenericPeakDetector.h)
- [Hardware original](../../../hardware/node/GenericPeakDetector.cpp)
- [Capture bridge](../../firmware/FireBeetle_to_Ultra96/components/hardware_core/src/hardware_bridge.cpp)
- [Firmware physical pipeline](../../firmware/FireBeetle_to_Ultra96/src/physical_pipeline.c)
- [Ultra96 server](../../ultra96/server.py)
- [Ultra96 AI server](../../ultra96/server_AI.py)
- [Checked-in window assembler](../../ultra96/window_assembler.py)

At inspection time, `window_assembler_updated.py`, `inference_pipeline.py`,
`late_fusion_vart.py`, `dpu_runtime.py`, and `config.py`, referenced by the AI
server, were absent from `communications/ultra96`. The deployed dependencies
must be reviewed before claiming end-to-end single-node AI support.

## Readiness and synchronization model

Distinguish supported nodes, connected nodes, synchronized/ready nodes, and
acknowledged streaming nodes. A connected node is not automatically eligible
for capture or fusion. An unready partner must not block a ready node.

Keep Ultra96 as the common time reference:

```text
Ultra96 sample time = node local sample time - node-minus-Ultra96 offset
```

Keep the existing offset calculation, synchronization quality checks, and
periodic refresh behavior. Change the lifecycle so that each node can become
ready independently. Joining a second node must not reset the first node's
mapping or the run's window origin. Keep per-node session/boot identity and
capture-generation checks so reconnects cannot introduce stale samples or ACKs.

The run's window origin is distinct from the time a late node starts sending.
A joining node contributes samples to existing capture-time intervals. It
cannot recreate samples from before it booted or outside its retained history.

## Planned scenario sequences

### 1. Node 1 connects first; node 2 stays offline

```text
Node 1 connects
-> Synchronizes with Ultra96
-> Ultra96 arms node 1
-> Node 1 detects a generic peak
-> Ultra96 starts its history flush and live stream
-> Node 1 windows go to single-node AI inference
-> Operation continues without node 2
```

### 2. Node 2 connects first; node 1 stays offline

```text
Node 2 connects
-> Synchronizes with Ultra96
-> Ultra96 arms node 2
-> Node 2 detects a generic peak
-> Ultra96 starts its history flush and live stream
-> Node 2 windows go to single-node AI inference
-> Operation continues without node 1
```

This requires a node-2-only AI path. Hip-specific feedback and fall monitoring
are unavailable while node 1 is absent.

### 3. Both nodes are ready before activity

```text
Both nodes connect and synchronize independently
-> Ultra96 arms each ready node
-> Either node detects a generic peak
-> Ultra96 maps that peak onto its shared timeline
-> Ultra96 requests aligned history flushes from both nodes
-> Each node acknowledges and streams
-> Eligible two-node windows go to fusion inference
```

Only one triggering peak is needed. If one node fails to start, the other
proceeds once its own start is acknowledged; its progress must not depend on
the failed node's ACK. Late/failed commands remain isolated to their node.

### 4. A second node joins an active single-node capture

```text
Node 1 streams; single-node inference runs
-> Node 2 connects
-> Node 1 continues uninterrupted
-> Node 2 synchronizes with Ultra96
-> Ultra96 starts node 2 from a valid recent history/live boundary
-> Its samples are mapped onto the existing run timeline
-> Windows without sufficient node 2 coverage still use node 1 alone
-> Eligible windows containing both streams use fusion inference
```

The same sequence applies with node roles reversed. No new peak or global
restart is required. Do not resend the original peak's historical boundary:
it may precede the joining board's boot or retained buffer.

For example, if node 2 joins 30 seconds into the capture, earlier windows
remain single-node windows. New windows become eligible for fusion only when
node 2 has enough valid coverage. Do not replay already processed windows just
because another node has joined.

### 5. The second node joins before activity

```text
Node 1 is synchronized and armed
-> Node 2 connects and synchronizes
-> Ultra96 arms node 2 as well
-> Either node detects a peak
-> Ultra96 starts the nodes ready at that point
```

If a peak occurs while node 2 is still synchronizing, node 1 starts immediately.
Node 2 then follows the active-capture join sequence. Node 1's valid trigger
must not be invalidated merely by arming node 2.

### 6. One node disconnects or stalls during two-node streaming

```text
Both nodes stream
-> Node 2 disconnects or exceeds its liveness deadline
-> Node 1 continues streaming
-> Windows sufficiently covered by both may still use fusion
-> Subsequent windows use node 1 alone
-> Node 2 reconnects/recovers and obtains a valid mapping
-> It rejoins the existing timeline
-> Fusion resumes when window coverage permits
```

A bounded wait must prevent a connected but silent node from blocking window
production indefinitely. Preserve usable buffered samples while respecting
session identity and quality checks. Apply the same behavior in reverse.

### 7. Both nodes disconnect

```text
The last streaming node disconnects
-> Ultra96 ends the capture and stops producing new inference windows
-> Server stays available for connections
-> Either node reconnects and synchronizes
-> Ultra96 arms it
-> A fresh peak starts a new capture
```

The proposed default is a fresh capture after losing all nodes, rather than
stitching windows across a period with no measurements. A manual STOP still
takes precedence: reconnecting must not override an explicit stopped state.

### 8. Activity becomes idle, then resumes

```text
AI confirms idle from available valid input
-> Ultra96 stops transmission and rearms ready nodes
-> Nodes continue sampling locally and retaining history
-> Either armed node detects fresh activity
-> Ultra96 starts a new capture with the ready nodes
```

A node joining during idle synchronizes and becomes armed. Connection alone
does not start transmission. Missing data must not be treated as an idle
classification, and stale results from a previous capture must not pause a
new capture. Single-node idle classification needs AI validation too.

## Implementation scope

### Coordinator and control lifecycle

- Update both server variants to allow capture with at least one ready node.
- Separate initial capture start, joining an active capture, and node removal.
- Track per-node pending commands, acknowledgements, deadlines, and eligibility.
- Preserve the active run when a partner connects, disconnects, or fails sync.
- Preserve manual STOP, idle rearming, stale-message rejection, and clock
  mapping quality checks.
- Do not replace `(1, 2)` with a permanently fixed `(1,)`: either node must be
  able to start alone, and membership must change during a run.

### Firmware and join protocol

Define a join operation that can start a late node without another activity
trigger. Decide whether to reuse `START_FLUSH` with a valid recent boundary
or extend the protocol for a live-only start. Handle insufficient history
without stopping the existing stream. Retain the existing sampling cadence,
local detector, capture timestamps, and history-to-live ordering guarantees.

The current protocol does not accept zero-duration history in `START_FLUSH`.
A live-only option therefore requires an explicit implementation change.

### Window assembly and AI input

- Keep one run-wide window origin and configured window/hop durations.
- Produce windows from available eligible streams, with a bounded lateness policy.
- Report node presence, source identity, coverage, and per-node quality.
- Allow partial joining-node windows to fall back to the usable node.
- Keep an absent node distinct from small packet-loss padding in a present stream.
- Define behavior when neither node has acceptable window data: reject/skip
  inference rather than fabricate a valid measurement.

| Usable window input | Intended inference mode |
|---|---|
| Node 1 only | Node 1 single-node classification |
| Node 2 only | Node 2 single-node classification |
| Both nodes | Fusion classification |
| Neither | No valid inference |

The AI teammate must verify whether the existing model branches produce
independent classifications or require training/additional output heads.
Do not assume that zero-filling a completely missing node is a valid model
input unless that behavior has been trained and validated.

### Relay and output integration

Verify that the relay forwards either producer independently and supports
reconnection without a cross-node prerequisite. The inspected relay uses
per-connection forwarding, but end-to-end transitions still need testing.
Deliver feedback to available applicable nodes and keep role-specific behavior.

## Decisions to settle during implementation

1. Recent-history join versus a protocol-supported live-only join.
2. Per-node readiness, join ACK, liveness, and window-lateness deadlines.
3. Window coverage thresholds for admission to single-node/fusion inference.
4. Exact presence/quality metadata contract with the deployed AI pipeline.
5. Single-node model outputs, confidence handling, idle policy, and measured
   accuracy compared with fusion.

These details are unresolved; this plan does not prescribe unverified numerical
timeouts or claim compatibility with the missing AI dependencies.

## Acceptance checks

- Node 1 alone starts, triggers, produces windows, and runs its AI path.
- Node 2 alone passes the equivalent sequence.
- Both ready nodes start from one peak and produce aligned fusion windows.
- A late node synchronizes and joins without interrupting the first stream or
  resetting window IDs/timeline within the active capture.
- A node joining during arming, start ACK handling, or idle does not invalidate
  the existing node's valid commands or trigger.
- Failed synchronization, insufficient join history, or a missing ACK affects
  only the failing node while another usable stream exists.
- A disconnected or stalled node cannot block remaining-node inference.
- Reconnection does not admit old-session samples, ACKs, or inference results.
- Loss of all streams ends capture; reconnection follows the fresh-peak policy.
- Manual STOP remains stopped across connection changes.
- Idle/resume works with either node alone and with both, using validated AI
  outputs rather than missing-data assumptions.
- Two-node inference quality is measured against each single-node baseline.

## Suggested implementation order

1. Review the deployed AI dependencies and agree on the input/output contract.
2. Implement independent readiness and single-node capture/window production.
3. Implement and verify the late-node join protocol and timeline handling.
4. Add node-loss fallback, bounded waits, and reconnect handling.
5. Integrate validated single-node/fusion inference and feedback/idle behavior.
6. Run automated transition checks and physical one-/two-node acceptance tests.
