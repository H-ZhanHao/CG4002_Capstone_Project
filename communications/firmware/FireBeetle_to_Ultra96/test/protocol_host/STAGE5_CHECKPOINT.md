# Stage 5 checkpoint: local Hip fall alarm and ESP-to-Ultra96 reporting

> **Historical checkpoint - reduced Stage 5 local fall reporting.**
> This 100 Hz revision covers local Hip detection/alarm and ESP-to-Ultra96 receipt/
> ACK only. Its physical checks remained pending and phone fall forwarding was
> deliberately deferred at this checkpoint. Later phone events/ACKs and final
> server-controlled detection were implemented separately; those implementations
> do not complete this record's pending physical tests. The old
> detector adapter's gap/expiry rules and output patterns below are revision-
> specific and must not be treated as the restored final detector contract.
> Commands and deployment lists require matching historical source/images. See
> [current setup references](../../../../docs/README.md#component-guides) and
> [the consolidated checkpoint results](../../../../docs/EXPERIMENTS_AND_RESULTS.md#6-feedback-idle-and-fall-checkpoints).

The reduced Stage 5 scope is implemented and host tested. Physical verification
is pending. AR reporting, gateway/phone JSON and Unity changes are explicitly
deferred at the user's request. The full original Stage 5 is therefore not
complete. No board flashing, Ultra96 deployment, commits or external AR edits
were performed.

The user confirmed Stage 4 physical behavior: one Hip idle pattern per idle
episode, no repetition while idle, Stage 3 regression behavior unchanged, and
manual STOP/disconnect checks passed. Actual teammate AR-counter/APK integration
is not claimed tested.

## Local behavior and ownership

The communications component compiles the existing canonical
`hardware/node/FallDetector.cpp` unchanged. The communications-owned
`fall_bridge.cpp` receives the same fresh Hip acceleration readings already
captured by the pipeline. There is no second MPU6050 read. Node 2 never detects
or publishes falls and never drives the buzzer GPIO.

Canonical rules remain acceleration magnitude <4 m/s^2 for at least 100 ms,
then magnitude >25 m/s^2 within the one-second impact window. The adapter resets
pending evidence on invalid/nonfinite acceleration, non-increasing timestamps,
or a sampling gap >50 ms. It also checks impact-window expiry before invoking
the canonical detector, whose implementation otherwise checks impact before
timeout. This prevents a late impact completing an expired candidate without
changing standalone hardware behavior.

Detection runs while streaming, standby/armed, manual STOP, idle pause, and
network disconnection. A consumed event immediately requests the existing
nonblocking 3,000 ms fall alarm through the pipeline-owned feedback controller.
Fall overrides/discards an idle pattern. The adapter/reporting path suppresses
new alarms during the current three-second alarm, avoiding a sustained impact
or rapid synthetic commands repeatedly extending it. A later independent
sequence can generate another event.

Alarm output is requested before queue insertion. A full queue, no connection,
missing ACK, expiry or Ultra96 failure cannot prevent or prolong that alarm.
Report retries never invoke the buzzer. LED behavior and acquisition/history
settings are unchanged. No automatic STOP or new ready-LED trigger was added.

## Binary contract

There are now 25 binary packet types, with two new types:

| Type | ID | Direction | Payload | Frame |
| --- | --- | --- | --- | --- |
| DEVICE_EVENT | 0x13 | Hip -> Ultra96 | 32 bytes | 56 bytes |
| DEVICE_EVENT_ACK | 0x14 | Ultra96 -> Hip | 16 bytes | 40 bytes |

The existing 24-byte header and protocol version 1 are unchanged. All multibyte
fields use network byte order. Event header node=1, flags=0, current nonzero TLS
transport Session, sequence=low 32 bits of event ID, timestamp=occurrence time.

DEVICE_EVENT payload offsets (relative to payload, not frame):

| Offset | Width | Field |
| --- | --- | --- |
| 0 | u8 | kind=1, FALL_STARTED |
| 1 | u8 | synthetic=0 detector / 1 explicit test injection |
| 2 | u16 | reserved=0 |
| 4 | u32 | boot_id, same as HELLO |
| 8 | u64 | event_id, nonzero increasing per boot |
| 16 | u64 | occurred_us, ESP monotonic microseconds, equals header timestamp |
| 24 | u32 | alarm_duration_ms=3000 |
| 28 | u32 | reserved=0 |

ACK payload: boot_id:u32 at 0; event_id:u64 at 4; result:u8 at 12;
reserved three zero bytes at 13. Result 0=accepted, 1=duplicate. ACK header node=0,
flags=0, current Session, sequence=low 32 bits of event ID, and server send time.
Malformed events/identity conflicts produce the existing ERROR/disconnect path;
there is no success ACK for rejected data. Existing CONTROL_ACK remains unchanged.

The transport validates current Session before handing a DEVICE_EVENT_ACK to
the event queue. Boot/event identity and a prior send attempt must also match
before a pending event can be removed. ACK loss yields the same event again,
possibly wrapped in a new transport Session after reconnect.

## Delivery bounds and limitations

- ESP queue: eight fixed slots, separate from samples and application responses.
- Lifetime: 30 seconds from detection, including time offline; no indefinite
  backlog. Expired events are removed even while disconnected.
- Retry interval: 500 ms, at most 60 attempts; actual send timing also depends
  on the existing transport task and connection availability.
- On capacity exhaustion, preserve existing pending events and drop the newest
  report. Local alarm still runs. `DEVICE_EVENT_LOSS` logs cumulative expired and
  overflow counters. The local FALL_STARTED log includes `queued=0` on failure.
- STOP, ARM and Session changes do not clear this queue. ESP reboot does clear
  RAM; a new random boot identity prevents new events colliding with old ones.
- The existing TCP/TLS task owns all socket writes. Queue access uses bounded
  critical sections; GPIO, logging and socket I/O happen outside those sections.
- Both server entrypoints validate authenticated node=Hip, current connection,
  Session, HELLO boot, exact lengths/fields and contents before ACKing.
- Receiver deduplication is by node/boot/event, independent of transport Session.
  Each server coordinator retains up to 64 boot identities and 64 recent
  fingerprints per boot, plus the event high-water mark. Evicted old event IDs
  and exhausted boot capacity fail closed. Boot identities are not evicted to
  accidentally permit replay; capacity exhaustion requires a server restart.
- Receipt state is memory-only for that Ultra96 server process. Restarting the
  server loses deduplication state; a still-pending event can be accepted/logged
  again with the same identity. This is bounded reporting, not durable storage.
- ACK means validated receipt recorded by Ultra96. It does not prove GPIO
  operation or delivery to AR. No AR notification exists in this scope.

Ultra96 logs one `DEVICE_EVENT_ACCEPTED ... kind=FALL_STARTED ... synthetic=...`
per accepted logical event within its receipt lifetime, then
`DEVICE_EVENT_ACK_SENT ... result=accepted|duplicate`. Duplicates are ACKed but
do not create a second acceptance record. Logs explicitly say
`ar_reporting=deferred`. No fall event is inferred by parsing buzzer/log output.

## Synthetic diagnostic

On Hip USB serial (115200 baud, newline terminated), `fall inject` requests the
same alarm/reporting path with synthetic=1. It is explicitly a test injection,
not a sensor detection. Node 2 rejects it. Repeating within three seconds is
suppressed. The parser and per-loop serial work remain bounded.

`feedback fall-alarm` still tests output only and must never enqueue a fall
event. Existing rep/ready/idle feedback commands retain their behavior.

## Automated verification

| Check | Result |
| --- | --- |
| Stage 1 firmware host suites | 7 PASS |
| Stage 2 C protocol/delivery suite | PASS |
| Existing Python regressions | 129 OK; one optional AI config subtest skipped |
| Managed C# protocol checks | 13 PASS; existing reference-version warnings |
| Stage 3 WSS/rep integration | 24 PASS for both server entrypoints |
| Stage 4 idle integration | 15 PASS |
| Stage 5 detector/adapter, event queue/codecs, synthetic diagnostic | PASS |
| Stage 5 Python/C-peer integrations | 11 PASS, including both real server handlers |
| node1_stage5 | PASS; RAM 78,480 bytes; firmware flash 870,179 bytes |
| node2_stage5 | PASS; RAM 78,480 bytes; firmware flash 870,183 bytes |

The Stage 5 cases cover valid detection, short free fall, impact alone, expired
impact window, invalid readings, timestamp gaps/regression, cooldown, Hip-only
behavior, timer rollover, alarm priority/expiry, synthetic routing versus
output-only tests, queue overflow/expiry/retry bounds, exact C/Python bytes,
malformed fields, conflicting identities, delayed duplicate/reconnect, new boot,
receipt bounds, and lost ACK followed by retry through each actual binary server
handler. Socket tests fragment frames and verify no capture/feedback/phone
publication side effects. Closed-socket messages during replacement are expected.

The full regression run used OS TLS access for the existing C# WSS cases.
Unity Editor/Android, real DPU inference, physical fall detection/GPIO timing and
on-device sampling performance were not run. No missing-sample tuning was made.

From repository root:

```powershell
$env:PYTHONPATH = "$PWD/.build/stage2_python_deps"
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage5_tests.ps1 -PythonCommand D:/PlatformIO/penv/Scripts/python.exe
# New Stage 5 checks only:
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage5_tests.ps1 -PythonCommand D:/PlatformIO/penv/Scripts/python.exe -OnlyStage5
```

## Profiles, preservation and archive

`node1_stage5` / `node2_stage5` inherit their Stage 4 environments, with matching
`sdkconfig.node1_stage5` / `sdkconfig.node2_stage5` copied from Stage 4. Earlier
profiles/environment definitions remain unchanged. No stage-selection/fallback
guard was added. Canonical `hardware/` source is unchanged.

Before building, 123 earlier archive/profile files were SHA-256 recorded and
preexisting `.pio/build` bins/ELFs/configs saved outside that directory under
`.build/stage5_validation/prebuild_artifacts/`. Earlier archives were retained.

Bundle: `.build/firmware_stages/stage5_local_2026-11-01/`. Each node directory
contains bootloader/partition/firmware binaries, matching sdkconfig, flash script
and SHA-256 manifest. `source.zip` is the matching source snapshot; `validation/`
contains checks/build logs and preservation verification. Root `manifest.json`
covers bundle artifacts. `archive_stage5.py` saves each node immediately after
its build and refuses to overwrite node/source/manifest archives.

## Ultra96 copy list and physical test

> **Historical procedure and unresolved acceptance:** The copy/flash commands
> below are for the reduced Stage 5 snapshot, not the current server/firmware.
> This record does not contain physical confirmation of these checks. Synthetic
> injection verifies alarm/report delivery, not physical detector sensitivity.

For your existing Stage 4 setup, copy the updated `communications/ultra96/server_AI.py`
and new `communications/ultra96/device_events.py` together. It is fine to keep
your deployed server named `server_stage5.py`. If using the communications-only
server, copy updated `server.py` plus `device_events.py` instead.
Keep the working `inference_pipeline.py`, intended `window_assembler_updated.py`,
and matching Stage 3 visualizer protocol/rep/feedback modules. No gateway, Unity
or phone protocol update is required; no new listening/tunnel port is added.
The default AI log path remains `/home/xilinx/comms/logs/server.log`, overridable
with `--log-file`.

1. User action only: flash saved node1_stage5 to Hip and node2_stage5 to wrist:

   ```powershell
   $env:PLATFORMIO_CORE_DIR = 'D:/PlatformIO'
   & .build/firmware_stages/stage5_local_2026-11-01/node1_stage5/flash.ps1 -Port COM_HIP
   & .build/firmware_stages/stage5_local_2026-11-01/node2_stage5/flash.ps1 -Port COM_WRIST
   ```

2. Back up the deployed server, copy the two files above, and run exactly one:

   ```sh
   python3 server_stage5.py --sample-rate-hz 100 --rep-control-port 9101
   # Or server_AI.py if you kept the repository filename.
   ```

3. Keep Hip USB serial open at 115200 baud. After connection, send `fall inject`.
   Expect one three-second Hip alarm, local synthetic/queued log, one Ultra96
   `DEVICE_EVENT_ACCEPTED ... synthetic=True`, and an ACK matched by Hip.
   No phone fall alert is expected. The wrist must remain silent.

4. Repeat `fall inject` after >3 seconds: expect a new event ID and alarm.
   Repeat during an alarm: expect suppression rather than extension. Run
   `feedback fall-alarm`: expect the same output duration but NO accepted event
   on Ultra96. Run `fall inject` on wrist: expect rejection and no alarm/report.

5. Enter server console `stop`, then inject on Hip. Alarm/report/ACK must still
   work while capture stays stopped. Resume activity with `start` and confirm
   normal history flush/streaming and Stage 4 idle behavior.

6. Stop the Ultra96 server while keeping Hip powered, then inject: the local
   alarm must still stop after three seconds. Restart/reconnect within 30 seconds
   if possible: the still-pending report should arrive with its original ID and
   synthetic flag, without sounding again on delivery. Existing exponential
   reconnect backoff counts against the 30-second expiry.

7. Repeat but keep the server unavailable for >30 seconds: expect local alarm
   as usual, an ESP expiry counter increment, and no stale report after reconnect.
   A fresh injection after connection should work normally. Loss of power clears
   pending RAM reports; a rebooted board uses a new boot identity.

8. Repeat Stage 3 rep duplicate/count-jump/reconnect checks and Stage 4's two idle
   episodes plus manual STOP/disconnect checks. For priority, issue `feedback idle`
   then promptly `fall inject`: fall output should take over for three seconds,
   and the interrupted idle pattern should not resume.

Test real detector sequences only with a controlled sensor fixture; no person
needs to fall. Synthetic injection verifies the output/report path, not physical
detector sensitivity. Host tests feed deterministic acceleration sequences through
the canonical detector. Record those results separately from board observations.

Stop for the user's physical results. AR forwarding and its JSON contract remain
deferred until discussed with the AR teammate and explicitly authorized.
