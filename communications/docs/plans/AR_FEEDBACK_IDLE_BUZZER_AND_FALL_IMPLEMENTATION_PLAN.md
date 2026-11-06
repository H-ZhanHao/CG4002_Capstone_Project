# AR Feedback, Idle Buzzer, and Fall Reporting Implementation Plan

> Output behavior superseded on 2026-11-05: steady activity LEDs on both nodes,
> Hip-only rep beeps, silent idle and no ready flashes. See
> [the current integrated contract](../../ultra96/README.md#integrated-activity-and-rep-outputs).
> The stage descriptions below remain a historical implementation record.

Status: Stages 1 and 2 physical checkpoints confirmed by the user. Stage 3
implementation, automated checks, and node1_stage3/node2_stage3 builds complete;
Stage 3 simulator physical cases passed per user; actual AR caller/APK testing
remains pending with the Unity teammate. Stage 4 physical idle, STOP/disconnect
and Stage 3 regression behavior were confirmed by the user. Stage 5 local alarm
and ESP-to-Ultra96 reporting are implemented/host tested; physical testing is
pending. Stage 5 AR reporting is explicitly deferred.
Bluetooth remains removed from the communications runtime to recover RAM for
Wi-Fi/TLS.

This brief is intended for GPT-6.1 Sol with medium reasoning. Implement one
stage at a time, verify it, and stop at its checkpoint before proceeding.

## 1. Objective

- AR/phone owns rep counting.
- Every new AR rep triggers a 200 ms LED pulse on both ESPs.
- An AI-confirmed idle transition triggers a distinctive short buzzer pattern
  on Node 1/Hip.
- A locally detected fall immediately triggers the Hip buzzer for three seconds
  and sends an explicit event to AR.
- Ready feedback can reuse the hardware's three-blink pattern.

Complete each stage's automated checks and provide physical test instructions.
Wait for the user's test result and instruction before implementing the next
stage. Compilation alone does not verify physical behavior.

## 2. Working Rules

- Inspect repository instructions and the current working tree before editing.
  Preserve existing uncommitted work.
- Keep the teammate's standalone `hardware/` application unchanged. Add
  communications-owned adapters and reuse suitable hardware modules.
- Retain all existing profiles, including `node1_integration_100HZ`,
  `node2_integration_100HZ`, and every earlier stage profile. Do not rewrite,
  rename, repurpose, or change their saved configuration or build flags when
  implementing a later stage.
- Each stage X must create a separate pair of PlatformIO environments
  `node1_stageX` and `node2_stageX`, with matching configuration files
  `sdkconfig.node1_stageX` and `sdkconfig.node2_stageX`. Retain the already
  created Stage 1 pair. Stage 1 starts from the original 100 Hz baseline;
  subsequent pairs carry forward the preceding stage's verified behavior and
  add only the current stage's features.
- Keep the active firmware implementation cumulative and straightforward.
  Do not accumulate stage-selection flags, conditional fallback branches, or
  duplicate legacy implementations merely to rebuild earlier stages from the
  latest shared source. Normal hardware/node configuration is still allowed.
- Profiles store build settings; they do not freeze shared source code.
  Preserve the matching source snapshot for each verified stage, or record its
  matching Git revision once committed by the user. Rebuild an earlier stage
  from that source version with its retained profiles, not from the latest
  source. Do not automatically create commits or tags.
- Build and physically test the current stage using its new profile pair.
  Run regression tests for the functionality carried forward from prior stages.
  Save each verified pair's flash artifacts and checksums in a separate
  stage-specific archive outside `.pio/build` so subsequent builds cannot
  overwrite the retained images. Earlier firmware can be flashed directly from
  those saved artifacts without retaining fallback code in the active source.
- Preserve 100 Hz acquisition, three-second windows, seven-second retained
  history, generic peak detection, clock synchronization, and activity-triggered
  streaming.
- `STOP` pauses network sample transmission; acquisition and local history
  continue.
- Do not add ESP-NOW LED mirroring. Ultra96 delivers feedback directly to both
  ESPs.
- Do not automatically commit, flash boards, deploy to Ultra96, or edit the
  teammate's external AR project.
- At the end of each stage, report changed files, checks performed, remaining
  limitations, and the next physical test.

## 3. Architecture and Code Boundaries

### Rep Feedback

```text
AR phone
  -> existing secure WebSocket
  -> visualizer_gateway.py on Ultra96
  -> new loopback control interface
  -> server.py
  -> existing TLS connection to each ESP
  -> local LED pulse
```

### Idle Feedback

```text
AI consecutive-idle threshold
  -> coordinator accepts a new idle pause
  -> IDLE_BUZZ to Hip
  -> existing STOP -> ACK -> ARM flow
```

### Fall Feedback

```text
Hip accelerometer
  -> hardware FallDetector
  -> immediate local three-second buzzer
  -> DEVICE_EVENT
  -> Ultra96
  -> gateway
  -> AR fall alert
```

Logs remain diagnostic output. Never parse logs to detect or transmit a fall.

### Relevant Locations

All paths below are relative to the repository root.

| Location | Responsibility |
| --- | --- |
| `hardware/node/` | Canonical standalone hardware code; preserve it. |
| `communications/firmware/FireBeetle_to_Ultra96/` | Communications firmware project. |
| Firmware `components/hardware_core/` | Hardware modules and communications-owned C/C++ adapters. |
| Firmware `src/physical_pipeline.c` | Acquisition loop and ownership of hardware state. |
| Firmware `include/protocol.h` and `src/protocol.c` | ESP binary protocol definitions and codecs. |
| Firmware `src/application.c` and `src/tcp_client.c` | Command dispatch, responses, and TLS transport. |
| Firmware `test/protocol_host/` | Desktop tests for firmware logic and codecs. |
| `communications/ultra96/server.py` | ESP connections, session coordination, and feedback delivery. |
| `communications/ultra96/visualizer_gateway.py` | Secure phone connection and local message forwarding. |
| `communications/ultra96/visualizer_protocol.py` | Phone-facing message validation. |
| `communications/unity/Ultra96VisualizerDemo/Assets/Scripts/Networking/` | Checked-in Unity networking client and contracts. |

The hardware LED timing can be reused, but its current `NodeSession` trigger
depends on an active session and peer handshake. The integrated feedback
controller must operate independently of that handshake. Local hardware counts
must not independently trigger LEDs when AR owns rep counting.

## 4. Stage Overview

| Stage | New profile pair | Outcome | Checkpoint |
| --- | --- | --- | --- |
| 1 | `node1_stage1`, `node2_stage1` | Independent local LED/buzzer patterns | Trigger and inspect outputs locally. |
| 2 | `node1_stage2`, `node2_stage2` | Ultra96-to-ESP feedback commands | Trigger outputs over TLS. |
| 3 | `node1_stage3`, `node2_stage3` | AR rep events through the gateway | Phone/simulator rep produces one pulse per ESP. |
| 4 | `node1_stage4`, `node2_stage4` | AI idle-threshold feedback | One beep per confirmed idle episode. |
| 5 | `node1_stage5`, `node2_stage5` | Local fall detection and reporting | Immediate local alarm and one logical AR alert. |

## 5. Stage 1 - Independent Local LED and Buzzer Control

**Goal:** verify output patterns before introducing network commands.

### Implementation

- [x] Retain `node1_stage1` / `node2_stage1` and their matching
  `sdkconfig.node1_stage1` / `sdkconfig.node2_stage1` files for this stage.
  Preserve the original `node1_integration_100HZ` / `node2_integration_100HZ`
  profiles and their matching pre-Stage 1 source/flash artifacts. Build and
  test Stage 1 using its own pair; no legacy fallback branches are required.
- [x] Add a communications-owned feedback controller with a small C-compatible
  bridge.
- [x] Reuse the hardware timings:
  - Rep LED: on for 200 ms.
  - Ready LED: three cycles of 100 ms off, 100 ms on.
  - Fall buzzer: on for 3,000 ms.
- [x] Use this default idle pattern: two 100 ms beeps separated by 100 ms off.
- [x] Drive the existing LED GPIO on both nodes. Initialize and drive the buzzer
  GPIO only on Node 1/Hip.
- [x] Keep timers nonblocking. Update the controller every physical-pipeline
  loop, including when sensor reads fail or streaming is stopped.
- [x] Give fall buzzer output priority over idle output. Cancel an interrupted
  idle pattern; do not replay it after the fall alarm.
- [x] Let a rep pulse replace an ongoing ready pattern.
- [x] In the Stage 1 profiles, make this controller the sole owner of LED
  GPIO writes. Prevent the existing exercise bridge from overwriting its state
  or triggering independent rep/ready feedback. Preserve standalone hardware
  behavior.
- [x] Add a bounded USB serial diagnostic command adapter for rep, ready, idle,
  fall-alarm output, and feedback status. The former Bluetooth adapter has been
  removed; ESP-IDF serial logging alone does not provide command parsing.
  The fall-alarm command tests output only; it must not claim a real fall was
  detected.
- [x] Do not automatically play patterns at boot.

Do not directly call `NodeSession::showLedFeedback()` as the external trigger:
it requires an active session and peer handshake. Reuse its timings and behavior
through the independent controller.

### Automated Checks

- [x] Correct LED and buzzer durations, including timer wraparound.
- [x] Fall interrupts idle and remains active for the intended duration.
- [x] Outputs return off without another sensor sample.
- [x] Node 2 does not drive the buzzer pin.
- [x] Local hardware rep processing cannot produce an additional LED flash.
- [x] Both `node1_stage1` and `node2_stage1` build successfully. Preserve the
  original 100 Hz profiles with their matching source version and saved flash
  artifacts; do not require legacy code paths in the active Stage 1 source.

### Physical Checkpoint

Trigger every pattern locally. Confirm LED and buzzer behavior while sampling
and while network streaming is stopped.

**Stop here and wait for the user's test result and instruction.**

## 6. Stage 2 - Ultra96-to-ESP Feedback Commands

Physical checkpoint confirmed by the user. See firmware
`test/protocol_host/STAGE2_CHECKPOINT.md` for the exact wire contract, automated
results, retained Stage 1/2 source and flash archives, and manual test commands.
Stage 1 fallback guards have been removed as requested; old profile definitions
and saved images remain retained. No later-stage fallback branches are required.

**Goal:** trigger the tested patterns through the existing TLS connections.

### Implementation

- [x] Create `node1_stage2` / `node2_stage2` with matching
  `sdkconfig.node1_stage2` / `sdkconfig.node2_stage2` files, carrying forward
  Stage 1's verified behavior. Build and test this stage using the new pair.
  Retain all existing profile definitions and the source/flash artifacts that
  match each earlier stage. Do not add stage fallback branches to active code.
- [x] Add one binary message type, `FEEDBACK_COMMAND`, with actions `REP_LED`,
  `READY_LED`, and `IDLE_BUZZ`.
- [x] Check unused message IDs before selecting one. Document the exact payload
  layout, field widths, byte order, and validation rules in both C and Python.
- [x] Retain the existing header layout. Use its transport session and command
  sequence for delivery identity.
- [x] Include a logical feedback event identity that remains stable during
  retries.
- [x] Route commands through the pipeline's bounded queue. Only the pipeline
  task should modify feedback state or GPIOs.
- [x] Extend existing `CONTROL_ACK` encoding and handling to acknowledge feedback
  independently of pending START, STOP, and ARM commands.
- [x] Define acknowledgement as "accepted and applied/scheduled by firmware,"
  not proof that the physical LED or buzzer worked.
- [x] Add bounded retries and a deadline. Retrying a command must not restart
  its pulse or beep.
- [x] Reject stale sessions, malformed payloads, unsupported actions, and reuse
  of an identity with different contents. Deduplication must handle delayed
  duplicates after intervening commands, not just an immediately repeated
  packet.
- [x] Clear pending delivery on disconnect. Do not replay expired feedback
  after reconnect.
- [x] Add Ultra96 console test commands to pulse both LEDs, play the ready
  pattern on both nodes, and play idle feedback on Hip.
- [x] Make feedback work while sample transmission is stopped and independently
  of the local exercise activation handshake.

### Automated Checks

- [x] Build `node1_stage2` and `node2_stage2` and run regression tests for
  carried-forward behavior. Preserve the new pair's matching source snapshot
  (or user-committed revision), flash artifacts, and checksums separately.
  Earlier-stage rebuilds must use their matching source versions.
- [x] Independent expected-byte codec tests in C and Python.
- [x] Malformed, stale, duplicate, and conflicting commands.
- [x] Lost acknowledgement followed by retry produces one output.
- [x] Independent acknowledgements from both ESPs.
- [x] One node disconnects without blocking the other.
- [x] Existing acquisition and control-flow tests remain passing.

### Physical Checkpoint

Use Ultra96 console commands. Confirm both LED outputs, Hip-only idle beep,
acknowledgements, duplicate suppression, and continued sampling.

**Stop here and wait for the user's test result and instruction.**

## 7. Stage 3 - AR Rep Events Through the Gateway

Simulator physical checkpoint confirmed by the user; real AR caller/APK
integration remains pending with the Unity teammate. See firmware
`test/protocol_host/STAGE3_CHECKPOINT.md` for contracts, tests, retained flash
bundles, simulator commands, and the checked-in Unity caller API. Unity Editor
execution was license-blocked; managed compilation/protocol and actual C# WSS
transport tests passed. The external AR project remains unchanged.

**Goal:** connect phone-owned rep counting to the tested downlink.

### Implementation

- [x] Create `node1_stage3` / `node2_stage3` with matching
  `sdkconfig.node1_stage3` / `sdkconfig.node2_stage3` files, carrying forward
  Stage 2's verified behavior. Build and test this stage using the new pair.
  Retain all existing profile definitions and the source/flash artifacts that
  match each earlier stage. Do not add stage fallback branches to active code.
- [x] Extend the existing secure WebSocket connection for application messages
  in both directions.
- [x] Define a versioned `rep_event` contract containing:
  - `run_id`: identifies the active Ultra96 process.
  - `rep_session_id`: identifies an exercise set and changes when AR resets its
    count.
  - `seq`: orders phone messages.
  - `rep_index`: cumulative count within that rep session.
  - `exercise`.
- [x] Establish explicit rep-session start/reset behavior. Do not interpret a
  lower count as a new session automatically.
- [x] Add a bounded loopback-only control interface between gateway and
  `server.py`. Keep ESP TLS connections owned by `server.py`.
- [x] Validate message types, values, sizes, current session identity, and rate
  limits. Reject unsupported phone commands.
- [x] Forward each accepted new rep to both ESPs as `REP_LED`, preserving the
  same logical event identity through retries.
- [x] Treat a received count jump, such as 4 to 7, as one current feedback pulse.
  Do not queue three delayed flashes.
- [x] On reconnect, distinguish current-count synchronization from a new rep
  event. Do not flash for an old count or replay a backlog of reps.
- [x] Return delivery results that distinguish accepted, duplicate, rejected,
  applied on each node, and delivery timeout.
- [x] Provide a Unity API such as `SendRepEvent(...)`, plus an explicit
  rep-session API. Keep the rep-counting algorithm owned by the AR teammate.
- [x] Provide a small simulated phone client so communications can test without
  the final AR project.

### Automated Checks

- [x] Build `node1_stage3` and `node2_stage3` and run regression tests for
  carried-forward behavior. Preserve the new pair's matching source snapshot
  (or user-committed revision), flash artifacts, and checksums separately.
  Earlier-stage rebuilds must use their matching source versions.
- [x] Simulated phone to gateway to server to two simulated ESPs.
- [x] Same rep submitted twice, including with a new transport sequence.
- [x] Count reset in a new rep session.
- [x] Stale run, stale session, reconnect, skipped counts, and unavailable ESP.
- [x] Simultaneous incoming exercise updates and outgoing rep events.
- [x] Bounded queues and serialized WebSocket writes.

### Physical Checkpoint

Send simulated reps, then integrate the AR teammate's caller. Confirm one pulse
per new rep on each connected node and no replay flashes after reconnect.

**Stop here and wait for the user's test result and instruction.**

## 8. Stage 4 - AI Idle-Threshold Integration

**Goal:** play one Hip idle pattern when AI confirms an idle pause.

Implementation and host checks are recorded in firmware
`test/protocol_host/STAGE4_CHECKPOINT.md`. Physical checks passed per user,
including manual STOP/disconnect and unchanged Stage 3 simulator behavior. The AI
log default is `/home/xilinx/comms/logs/server.log`; `--log-file` overrides it.

### Implementation

- [x] Create `node1_stage4` / `node2_stage4` with matching
  `sdkconfig.node1_stage4` / `sdkconfig.node2_stage4` files, carrying forward
  Stage 3's verified behavior. Build and test this stage using the new pair.
  Retain all existing profile definitions and the source/flash artifacts that
  match each earlier stage. Do not add stage fallback branches to active code.
- [x] Locate the actual inference-enabled server before editing.
- [x] Verify the inference integration source: `server_AI.py` wires the existing
  `InferencePipeline.on_idle` to `handle_idle_classification`. The user supplied
  the Ultra96 pipeline source, retained only as a host-test fixture. Preserve
  the intentional `window_assembler_updated.py` reference; it is the locally
  updated assembler deployed under a separate name.
- [x] At the accepted transition into idle pause, create one logical `IDLE_BUZZ`
  event for Hip.
- [x] Use the existing AI threshold: five consecutive confident idle results,
  confidence >= 0.80. No new threshold or inference mechanism was added.
- [x] Send feedback independently of the existing STOP -> ACK -> ARM flow.
  A buzzer delivery failure must not prevent stopping or rearming.
- [x] Ensure further idle results, repeated `idle_pause()` calls, STOP retries,
  and ARM retries do not create another beep.
- [x] Reuse the existing inactive-generation guard and reset evidence on a fresh
  activity run; no separate notification latch is necessary.
  Validate that delayed results from the previous run cannot trigger another
  idle pause.
- [x] Ensure manual STOP, disconnects, and control timeouts do not accidentally
  trigger idle feedback.
- [x] Keep acquisition and retained history running during the pause.

### Automated Checks

- [x] Build `node1_stage4` and `node2_stage4` and run regression tests for
  carried-forward behavior. Preserve the new pair's matching source snapshot
  (or user-committed revision), flash artifacts, and checksums separately.
  Earlier-stage rebuilds must use their matching source versions.
- [x] Threshold reached produces one idle beep event.
- [x] Non-idle results reset the consecutive count.
- [x] Repeated idle and control retries produce no extra beep.
- [x] Resume followed by another idle episode produces one new beep.
- [x] Feedback failure does not block STOP/ARM.
- [x] Old inference results are ignored.

### Physical Checkpoint

Exercise -> confirmed idle -> one Hip beep -> activity resumes automatically.
Repeat for a second idle episode.

**Stop here and wait for the user's physical Stage 4 test result and instruction.**
No Stage 5 implementation is authorized by this checkpoint.

## 9. Stage 5 - Local Fall Detection and Reporting to AR

**Goal:** reuse the hardware fall detector and report its event explicitly.

**Authorized scope update:** implement local Hip detection/alarm and reliable
ESP-to-Ultra96 receipt/ACK first. AR JSON, gateway forwarding and Unity changes
are deferred pending the AR teammate discussion. See firmware
`test/protocol_host/STAGE5_CHECKPOINT.md` for the actual wire layout, bounds,
results, saved firmware/source and physical steps. Full Stage 5 is incomplete.

### Implementation

- [x] Create `node1_stage5` / `node2_stage5` with matching
  `sdkconfig.node1_stage5` / `sdkconfig.node2_stage5` files, carrying forward
  Stage 4's verified behavior. Build and test this stage using the new pair.
  Retain all existing profile definitions and the source/flash artifacts that
  match each earlier stage. Do not add stage fallback branches to active code.
- [x] Build/reuse the original hardware `FallDetector` through a communications
  adapter.
- [x] Feed it fresh valid Hip accelerometer samples from the existing single
  acquisition path. Do not read the MPU6050 a second time.
- [x] Preserve its current detection rules:
  - Acceleration magnitude below 4 m/s^2 for at least 100 ms.
  - Then an impact above 25 m/s^2 within the one-second impact window.
- [x] Keep monitoring during standby, idle pause, and network disconnection.
- [x] Prevent an invalid sample or long sampling interruption from joining
  unrelated free-fall and impact readings.
- [x] On a consumed fall event, immediately activate the local three-second
  fall alarm and create a structured event for transmission.
- [x] Add `DEVICE_EVENT` with `FALL_STARTED`. Include boot identity, event
  identity, occurrence timestamp, node identity, and alarm duration.
- [x] Implement bounded acknowledgement/retry delivery for fall events.
  Explicitly document the event acknowledgement mechanism; existing
  `CONTROL_ACK` currently serves Ultra96-to-ESP commands and must not be assumed
  to support reverse events automatically.
- [x] Use the same logical event identity for retries. Include boot identity so
  rebooted nodes cannot collide with earlier event IDs.
- [x] Use a bounded event queue separate from replaceable sample/exercise
  updates. STOP must not erase a pending fall event. Define expiry and overflow
  behavior.
- [ ] Add a versioned `fall_alert` JSON message through the gateway to AR.
  Deduplicate retries.
- [ ] Ensure the gateway's current latest-exercise slot cannot overwrite an
  undelivered fall alert.
- [x] Keep local alarm execution independent of network acknowledgement.
- [x] Implement binary `FALL_STARTED` with duration. No `FALL_ENDED` was added;
  AR contract decisions remain deferred.
- [x] Provide clearly marked synthetic fall injection for testing.

### Automated Checks

- [x] Build `node1_stage5` and `node2_stage5` and run regression tests for
  carried-forward behavior. Preserve the new pair's matching source snapshot
  (or user-committed revision), flash artifacts, and checksums separately.
  Earlier-stage rebuilds must use their matching source versions.
- [x] Free fall followed by valid impact produces one fall event.
- [x] Short low-acceleration period, impact alone, expired impact window, and
  sample gaps do not create a fall event from those sequences.
- [x] Fall interrupts idle buzzer output.
- [x] Event retries reach Ultra96 once logically within one server process,
  including lost ACK followed by a new transport Session.
- [ ] Event retries reach AR once logically (deferred).
- [x] Network failure does not prevent or prolong the local alarm.
- [ ] Events remain separate from exercise-update replacement.
- [x] Node reboot and reconnect do not create duplicate alerts.

### Physical Checkpoint

Use synthetic injection first to test the complete alarm/reporting path. Any
physical detector test should use a controlled sensor fixture; a person does
not need to fall.

**Stop here and wait for the user's test result and instruction.**

## 10. Final Regression and Handoff

Once all stages pass:

- [ ] Build `node1_stage5` and `node2_stage5` and check memory/flash usage.
- [ ] Confirm the original 100 Hz and earlier stage profiles have matching
  source snapshots/revision references and separately saved flash artifacts.
  Any earlier-stage rebuild must use its matching source version; the latest
  source is not required to contain earlier-stage fallback implementations.
- [ ] Run the affected protocol, history, detector, coordinator, gateway, and
  Unity tests.
- [ ] Verify 100 Hz acquisition, three-second windows, and idle auto-resumption
  with feedback enabled.
- [ ] Test simultaneous rep feedback, idle transition, and fall alarm.
- [ ] Document packet layouts, message contracts, expiry/retry rules,
  diagnostic commands, and deployment order.
- [ ] Record automated results separately from physical-board results.
- [ ] Hand the AR teammate the rep-session/rep-event API and fall-alert contract.
- [ ] Hand the AI teammate the exact idle-transition hook.

## 11. Stage Completion Report

At each checkpoint, provide:

1. Implemented behavior and changed files.
2. Current stage's environment names, sdkconfig filenames, automated checks,
   and build results, including failures or skipped checks. Confirm that all
   existing profile definitions remain retained. Give the matching source
   snapshot/revision reference, saved flash-artifact locations, and checksums
   for reproducing this stage without adding fallback branches to active code.
3. Exact commands and expected observations for the current physical test.
4. Any missing teammate source or integration dependency.
5. Explicit confirmation that the next stage has not started.

**Current checkpoint: reduced Stage 5 physical alarm/reporting tests.** Stage 4
physical idle, STOP/disconnect and Stage 3 regressions passed per the user.
Stage 5 local detection/alarm and ESP-to-Ultra96 reporting were authorized.
AR reporting/JSON/gateway/Unity work remains deferred until explicitly authorized.
