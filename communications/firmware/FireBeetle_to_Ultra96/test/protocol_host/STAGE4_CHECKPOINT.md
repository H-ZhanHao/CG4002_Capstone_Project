# Stage 4 checkpoint: AI idle feedback

> **Historical checkpoint - Stage 4 AI idle feedback.**
> This 100 Hz staged revision used the earlier Hip idle double-beep and phone rep
> LED flashes. The opening update records user-confirmed idle episodes, Stage 3
> regression and manual STOP/disconnect checks. The original bundle's pending
> statement and closing stop instruction describe the earlier build-only gate.
> Deterministic host inference tests did not run real DPU models, and teammate
> AR counter/APK acceptance remained unverified at this checkpoint. Commands and
> deployment lists require matching saved source/images. Final idle is silent.
> See [current setup references](../../../../docs/README.md#component-guides) and
> [the consolidated checkpoint results](../../../../docs/EXPERIMENTS_AND_RESULTS.md#6-feedback-idle-and-fall-checkpoints).

Stage 4 implementation, host verification and physical checks are complete.
The user confirmed idle episode behavior, Stage 3 regression behavior, and
manual STOP/disconnect checks. Reduced Stage 5 local alarm/reporting is now
documented separately in STAGE5_CHECKPOINT.md. No boards were flashed, files
deployed to Ultra96, commits created, or external Unity project files edited.
The user confirmed Stage 3 simulator physical cases all passed; the real AR
counter/updated APK is still with the Unity teammate and is not claimed tested.

## Behavior

`server_AI.py` retains the existing `InferencePipeline.on_idle` callback and
intentional `window_assembler_updated` import. On Ultra96 that module is the
updated local `window_assembler.py`, deliberately deployed under a separate name.
The user's supplied pipeline remains unchanged on Ultra96. A transcribed copy
under `communications/ultra96/stage4_fixtures/` is used only by host tests.

The existing policy requires five consecutive accepted, confident idle
classifications (confidence >= 0.80). Non-idle and low/invalid-confidence results
reset the count; rejected sensor windows do not increment or reset it. Five is a
classification count, not five seconds. The supplied comment mentioning ten is
outdated; its executable constant is five.

`handle_idle_classification` checks physical mode, running intent, active capture
and `accept_idle(result, generation)` under the coordinator lock. After acceptance
it invokes the existing `idle_pause()` and submits exactly one `IDLE_BUZZ` via
`request_feedback`. Node 1/Hip alone is targeted. One event plays the existing
two 100 ms beeps separated by 100 ms off, not a new buzzer pattern.

The existing STOP is sent and the pipeline invalidated before feedback is
submitted. Delivery runs through the existing asynchronous bounded outbox;
submission exceptions are logged as `IDLE_FEEDBACK_FAILED` without interrupting
STOP -> ACK -> ARM. Delivery timeouts also do not affect that control flow.
No new socket writer, retry policy, idle counter or stage guard was added.

Repeated results cannot reaccept the inactive generation. STOP/ARM handling and
direct `idle_pause()` retries do not submit feedback. On a fresh activity trigger,
START_FLUSH and its ACKs start a new pipeline generation with reset evidence,
allowing the next idle episode to create one new event. Stale in-flight and queued
inference is discarded. Manual STOP, console `idle`, disconnect and control
timeouts create no automatic buzzer event. Explicit `feedback idle` remains a
bench command and intentionally plays a pattern. Ready LEDs retain only their
existing explicit `feedback ready` trigger.

ESP sampling, rolling history, window construction and control behavior were
not changed. Stage 3 sessions, count deduplication, reconnect synchronization,
and local control port 9101 remain unchanged. `server.py` and standalone
`hardware/` are unchanged.

The AI server log default is `/home/xilinx/comms/logs/server.log`, independent of
root/home/current working directory. `--log-file` still overrides it. Directory
creation and rotation settings (10 MiB, five backups by default) are unchanged.

## Automated results

| Check | Result |
| --- | --- |
| Stage 1 firmware host executables | 7 PASS |
| Stage 2 production C codec/delivery tests | PASS |
| Existing Python regression suite | 129 OK, one optional AI configuration subtest skipped |
| Managed C# protocol checks | 13 PASS; installed Unity references, existing compatibility warnings |
| Stage 3 gateway/rep tests for both servers | 24 PASS, including C# WSS and simulator -> two production-C peers |
| Stage 4 integration tests | 15 PASS |
| `node1_stage4` | PASS; RAM 77,864 bytes; firmware flash 867,339 bytes |
| `node2_stage4` | PASS; RAM 77,864 bytes; firmware flash 867,343 bytes |

Stage 4 covers the fifth-result boundary, non-idle and invalid-confidence resets,
rejected windows, repeated callbacks/pause calls while STOP ACKs are pending,
ARM pending/ready, delivery retries, fresh-trigger resume, a second idle episode,
submission exceptions, feedback deadline expiry, manual controls, disconnect,
START/ARM timeouts, stale in-flight/queued work after resume, and logging override.
The two-C-peer case drops the Hip ACK, retries the same event and verifies one
Hip output, two command frames, and no wrist output/frame.

The first Stage 3 integration attempt failed two C# WSS cases because sandboxed
Windows TLS credential acquisition failed. Rerunning all 24 rep tests outside the
sandbox passed. No code was changed to bypass TLS. Logs are retained in the
Stage 4 bundle's `validation/` directory.

Reproduce from repository root (Windows GCC/G++, .NET and Unity references):

```powershell
$env:PYTHONPATH = "$PWD/.build/stage2_python_deps"
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage4_tests.ps1 -PythonCommand D:/PlatformIO/penv/Scripts/python.exe
```

The C# transport tests require usable OS TLS credentials; use an approved
unsandboxed run if sandbox restrictions deny them. `-SkipManaged` skips compiling
the managed test host; a preexisting host can still be exercised by rep tests.
Live DPU/VART inference, GPIO timing on physical boards, and Unity Editor/Android
AR execution were not run here. Stage 4 inference tests use deterministic model
results through the supplied real pipeline logic, not actual DPU models.

## Profiles and retained artifacts

New profiles are `sdkconfig.node1_stage4` / `sdkconfig.node2_stage4`, copied from
Stage 3; `node1_stage4` / `node2_stage4` inherit their corresponding Stage 3
environments. Existing environment sections and sdkconfig files remain unchanged.
The preexisting Stage 1 profile flags were retained verbatim as required; no new
stage-selection guard or fallback branch was introduced.

Before any build, 99 existing archive/profile files were SHA-256 recorded and
all preexisting `.pio/build` binaries/ELFs/configuration artifacts were copied to
`.build/stage4_validation/prebuild_artifacts/`. Earlier archives remain in place.
The preservation manifest is `.build/stage4_validation/preserved_sha256.json`.
The final preservation verification is included in `validation/preservation.txt`.

Stage 4 bundle: `.build/firmware_stages/stage4_2026-11-01/`:

- `source.zip`: matching communications/hardware sources, profiles and tests,
  including the supplied pipeline fixture; actual external DPU dependencies/models
  already on Ultra96 are not recreated or replaced.
- `node1_stage4/`, `node2_stage4/`: `bootloader.bin`, `partitions.bin`,
  `firmware.bin`, matching sdkconfig, flash script and SHA-256 manifest.
- `validation/`: test/build logs and preservation verification.
- Root `manifest.json`: SHA-256 checksums for all bundle artifacts.

Nodes are archived individually immediately after their build, before another
PlatformIO build can prune them. `archive_stage4.py` refuses to overwrite node
archives or final source/manifests. Flash saved images directly without rebuilding.
This bundle is build/host verified; physical Stage 4 verification is pending.

> **Status clarification:** The preceding sentence is the original bundle status.
> The opening update records subsequent user confirmation of the Stage 4 physical
> cases. That confirmation does not add a measured GPIO timing distribution,
> actual DPU-model execution or teammate AR caller/APK acceptance to this record.

## Ultra96 copy list

For an already deployed matching Stage 3 setup, copy **only the updated
`communications/ultra96/server_AI.py`** to the existing Ultra96 comms directory.
Keep the working `inference_pipeline.py`, `window_assembler_updated.py`,
`config.py`, `late_fusion_vart.py`, `dpu_runtime.py`, compiled models and overlay.
No pipeline update is required. Do not deploy `stage4_fixtures/`.

If aligning a mixed/older deployment, copy the existing matching Stage 3 support
files together: `feedback_delivery.py`, `rep_control.py`, `visualizer_protocol.py`,
and `visualizer_gateway.py`. These files have no Stage 4 code changes. Keep 9101
local to Ultra96; existing SSH/relay forwarding commands need no additional port.
`simulate_phone_reps.py` stays on the machine running the simulator.

## Physical Stage 4 test

> **Historical procedure:** These steps and expected beeps/flashes refer to the
> saved Stage 4 firmware and server. The user-confirmed cases are listed above;
> current final firmware uses different feedback actions and silent idle.

1. User action only: flash both saved Stage 4 images with the correct COM ports:

   ```powershell
   $env:PLATFORMIO_CORE_DIR = 'D:/PlatformIO'
   & .build/firmware_stages/stage4_2026-11-01/node1_stage4/flash.ps1 -Port COM_HIP
   & .build/firmware_stages/stage4_2026-11-01/node2_stage4/flash.ps1 -Port COM_WRIST
   ```

2. Back up the deployed AI server, copy the file above, then run exactly one
   server from its existing comms directory with its working DPU dependencies:

   ```sh
   python3 server_AI.py --sample-rate-hz 100 --rep-control-port 9101
   ```

   Check `/home/xilinx/comms/logs/server.log` receives logs even as root. To
   choose another location, add `--log-file /your/path/server.log`.
   Use the existing gateway TLS command/certificates if checking phone reps.

3. Wait for both nodes to synchronize and `ARM_CONFIRMED`. Expect no automatic
   ready pattern or idle sound at startup. Exercise to trigger streaming and
   `START_CONFIRMED`; then become idle. Watch `IDLE_STREAK ... required=5`.
   Counts 1-4 must not buzz. At 5 expect one Hip double-beep pattern, no wrist
   buzzer, STOP ACKs and a fresh ARM confirmation. Feedback may arrive before
   or after STOP ACKs; it must not delay rearming.

4. Remain idle after ARM. Expect no additional automatic pattern. Then resume
   movement, confirm a fresh START_FLUSH/START_CONFIRMED with historical samples,
   and become idle again. Expect exactly one new Hip double-beep at the next
   five-result confirmation. Rejected windows may lengthen the wait.

5. During activity use console `stop`. Expect no idle sound and no movement
   restart until `start`. A disconnect/reconnect must not create an idle event
   or replay old feedback. Acquisition/history continue while streaming is
   stopped; confirm historical data on the next activity start. Optional console
   `idle` tests pause/rearm only and must not create AI feedback.

6. Regression-check Stage 3 simulator counts `1,1,4`, reconnect synchronization
   and a new session: session/sync do not flash, duplicate 1 does not flash, new
   1 and jump 4 each flash both LEDs once. A disconnected node does not replay
   missed feedback after reconnect. Actual AR counter/APK integration remains
   a separate Unity teammate test.

7. Inspect `FEEDBACK_SENT`/`FEEDBACK_ACK` and per-event results. Retries keep one
   event identity. If delivery fails, STOP/ACK/ARM must still proceed; report the
   failure rather than interpreting an ACK as physical proof. Lost ACK and
   timeout behavior were injected in host tests, not on boards in this run.

Record both idle episodes, node outputs, log path and Stage 3 regression results.
Stop here for the user's physical results. Do not implement Stage 5 yet.
