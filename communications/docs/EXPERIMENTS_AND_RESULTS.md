# Communications experiments and results

Consolidated on 16 November 2026. This record collects existing evidence for the
design report; no firmware, runtime or physical tests were rerun during this
documentation consolidation. See [Design evolution](DESIGN_EVOLUTION.md) for
the development sequence and the nine source documents replaced by these records.

## 1. Evidence rules and tested configurations

**Recorded physical** means a retained handoff describes observations on boards
or a phone. **User-confirmed physical** means a checkpoint records the user's
confirmation, sometimes without raw measurements. **Automated** means a host,
simulator or loopback test. **Configured/calculated** means a source setting or
arithmetic, not an experimental outcome. **Pending** means the reviewed records
do not establish a completed check.

| Configuration | Scope and limits |
| --- | --- |
| 19 September campus test | Historical Hub 3/6 routing and approximately 50 Hz streams; plain-TCP private relay profiles were later replaced by TLS. |
| 27-29 September preparation for the 1 October Individual Subsystem Test | Physical FireBeetles with 50 Hz synthetic IMU production, interactive TLS laptop relay, SSH and simulated exercise events. Benchmarks use the frame sizes/byte counts recorded at that time. |
| October integration slice | Mock-testable acquisition/history/physical-unit integration; host/build verification does not establish MPU6050 or worn-sensor performance. |
| November feedback stages | Host tests and user-confirmed checkpoints as identified below. Earlier LED/buzzer patterns were subsequently replaced. |
| Final source configuration | 100 Hz physical capture, three-second windows, 0.5-second hop (configured to 0.5 for final demo, by default it is 1 second), seven-second history and final dynamic-node profiles. Settings alone do not prove rate, latency, accuracy or final-device acceptance. |

The historical records describe 40-byte benchmark DATA frames, whereas later
plans specify a 38-byte raw frame and the physical format is 50 bytes. Preserve
the recorded counts/bytes/rates with their revision; do not convert these
benchmarks into final physical-link claims using a different frame size.
Application throughput excludes TLS, TCP/IP, SSH and Wi-Fi overhead. Test-suite
counts below are separate checkpoints with overlapping coverage; do not add
them together as a unique total or a current full-suite pass.

## 2. Network route selection

Source: [NUS network test results](results/NUS_NETWORK_TEST_RESULTS.md), describing
the tested environment on 19 September 2026.

| Experiment | Procedure and recorded result | Conclusion and limitation |
| --- | --- | --- |
| ESP32 to laptop on campus Wi-Fi | Both associated; ESP32 TCP connection did not reach the healthy laptop listener. | Tested client path unusable; isolation, VLAN separation or firewall were possible causes, not individually identified. |
| ESP32 direct to Ultra96 port 5000 | Association succeeded; TCP did not complete and board-side capture saw no expected connection. | Failure preceded application protocol exchange; not proof that every campus route is blocked. |
| Embedded SSH feasibility | ESP32 resolved/reached the jump host on port 22 and received its SSH banner. A simple laptop jump-host forward to Ultra96's application port returned administratively prohibited. | Banner reachability was not a completed embedded SSH tunnel. The known route needed Ultra96 SSH through the jump host. |
| Private DIR-842 LAN plus laptop SSH bridge | Both hubs obtained independent sessions, queues drained to zero and streams remained approximately 50 Hz. | Established the selected route in that environment; the original test used historical plain-TCP profiles. |

The tested laptop private adapter was 192.168.0.2/24 without gateway or DNS;
Ethernet 192.168.0.3/24 was a backup. Campus Wi-Fi retained the default Internet/VPN
route. Historical board addresses were 192.168.0.104 and 192.168.0.132. Addresses
are deployment observations and must be checked against current leases.

## 3. Single-producer transport and file integrity

Sources: [Phases 1-6](handoffs/PHASE_1_TO_6_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md)
and [Phase 7](handoffs/PHASE_7_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md), recorded
27 September 2026.

| Check | Recorded result | Evidence class and interpretation |
| --- | --- | --- |
| Protocol codecs and build checks | Host protocol tests passed; the recorded firmware environments built. | Automated/build evidence for those revisions. |
| Request processing | Hub 3 changed ax from -1200 to -1197 and gx from -250 to -150; exact duplicates returned cached content; conflicting sequence reuse produced error 8. | Recorded physical diagnostic processing; synthetic IMU input. |
| TLS positive/negative test | Correct server identity connected. A wrong identity caused certificate verification failure before HELLO/DATA acceptance. | Recorded physical fail-closed identity test; not a complete security audit. |
| Sustained normal stream | At stream age 801.2 s: 40,107 frames, 1,604,280 bytes, average 16.02 kbit/s; current 48.9 packets/s. Gap, duplicate, out-of-order and timestamp-regression counters were zero. | Recorded physical board transport with 50 Hz mock source; run described as approximately 15 minutes. |
| Queue and heap | Representative queue 0; current heap 174,668 bytes, minimum 165,548 bytes, returning to stable levels. | No observed continuing decline in this run; does not prove leak absence under every workload. |
| Full throughput run | 10,002 ms; ESP and laptop each counted 31,920 frames / 1,276,800 bytes; laptop average 1,020.12 kbit/s; queue drops/send failures/integrity counters zero. | Recorded physical application benchmark, approximately 1.02 Mbit/s; not final 100 Hz sensor throughput. |
| Early benchmark stop | 2,100 ms; 2,720 frames / 108,800 bytes matched at ESP/laptop; average 409.69 kbit/s; no recorded losses. | Manual stop worked and normal streaming resumed. |
| Valid 64 KiB file | 65,536 bytes in 73 chunks; laptop, ESP and Ultra96 SHA-256 matched. Normal approximately 50 packets/s continued with zero reported integrity counters. | Recorded physical streaming transfer. |
| Corrupted first chunk | CRC rejection result 3; invalid chunk not forwarded; no successful file verification. | Recorded physical negative test. |
| Wrong first offset | Rejected with result 2; a later valid transfer succeeded after cleanup correction. | Recorded physical negative/recovery test. |
| Transfer inactivity | BEGIN without chunks timed out after approximately 10 s, result 6; next valid transfer succeeded in the same session. | Recorded physical timeout/state-cleanup test. |
| Interrupted 256 KiB file | Reset after approximately 160 of 292 chunks aborted the transfer. A fresh session/file ID restarted at offset zero and verified all 262,144 bytes. | Recorded physical reset recovery; no cross-session resume. |

The 64 KiB file SHA-256 was
`6276f7d9602ee9fbe7962449faae7e85fecdc8718e36be964f81360e80bc3c19`.
The 256 KiB file SHA-256 was
`978be40f8e9fd00e7e23e4bcf2befa5f11caa9b3e76b4d74ee64ca0f383e9ae6`.
These identify historical test content; they are not firmware checksums.

Issues found included a TLS-task stack overflow from the benchmark batch buffer,
benchmark counters accumulating across runs, and incomplete Ultra96 file state
surviving an ESP rejection. Bounded static workspaces, independent run resets
and terminal failure cleanup corrected those observed faults. Keep the source
handoffs for raw examples and detailed queue/heap observations.

## 4. Concurrent producers and recovery

Sources: [Phase 8](handoffs/PHASE_8_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md), recorded
28 September, and [Phase 9](handoffs/PHASE_9_PHYSICAL_TEST_RESULTS_AND_HANDOFF.md),
recorded 28 September 2026. All rates refer to the historical mock-data setup.

| Experiment | Recorded result | Limit |
| --- | --- | --- |
| Settled concurrent streams | Hub 6: 16.41 kbit/s, 51.3 packets/s; Hub 3: 16.29 kbit/s, 50.9 packets/s; combined 32.70 kbit/s, 102.2 packets/s; integrity counters zero. | Representative short-window readings, not a final physical sampling-rate distribution. |
| Targeted requests | Example Hub 6 latency 138.2 ms; Hub 3 latency 87.4 ms; transformations and response identities matched. | Individual diagnostic round trips; no percentile/end-to-end inference latency claim. |
| Per-producer replay state | Each board independently cached exact duplicates and rejected conflicting sequence reuse with error 8. | Recorded physical request tests. |
| Hub 3 individual benchmark | 35,024 frames / 1,400,960 bytes matched; 10,000 ms; laptop average 1,113.61 kbit/s. | Partner continued normal approximately 50 Hz traffic. |
| Hub 6 individual benchmark | 41,744 frames / 1,669,760 bytes matched; 10,003 ms; laptop average 1,331.76 kbit/s. | Partner continued normal traffic. |
| Concurrent benchmarks | Hub 6: 39,888 frames / 1,595,520 bytes, 1,275.04 kbit/s. Hub 3: 33,536 frames / 1,341,440 bytes, 1,068.39 kbit/s. | Each ESP/laptop total matched; reported drops, failures, gaps, duplicates and ordering faults zero. |
| Individual board resets | The unaffected board stayed near 50 Hz; the reset board reconnected with a fresh session. | Recorded physical isolation in both directions. |
| Power reset identity | Hub 3 boot ID changed from 3927540894 to 3990631657; session changed from 3405792474 to 3202014456. Hub 6 stayed live with zero integrity counters. | Distinguishes reboot from connection replacement. |
| Network-only loss | Hub 3 boot ID stayed 4140072089; session changed from 1847139766 to 3820799023. It reappeared approximately 64 s after disconnection. | Duration includes network absence; exact retry-delay transcript was not captured and this is not a reconnect-latency guarantee. |
| Interrupted benchmark/file | No successful completion under the interrupted session; neither operation resumed automatically; a fresh operation succeeded. | Recorded physical session-boundary checks. |
| Stale diagnostic keys | After reconnect, duplicate/conflict keys with no current request sent nothing; a fresh request restored normal diagnostics. | Cached requests did not cross sessions. |

File-transfer compatibility and clean two-board shutdown also passed in Phase 8.
Phase 9 testing found and corrected keyboard blocking during file transmission,
an invalid file path terminating keyboard handling, unintended diagnostic-key
fallback and an ambiguous old-connection close message during fast reconnect.
The latest recorded laptop suites progressed from 14 tests in Phase 8 to 16 in
Phase 9; those counts describe their respective snapshots.

The Phase 8 credential limitation remains part of the evidence: both hubs used
the same predictable development token. TLS and server verification were active,
but this did not demonstrate unique cryptographic identity per device.

## 5. Phone visualizer and isolated failures

Source: [Phase 10](handoffs/PHASE_10_IMPLEMENTATION_HANDOFF.md), recorded
28 September, supplemented by the consolidated visualizer progress snapshot
dated 29 September 2026.

The exercised route was Ultra96 producer to loopback publisher port 9100,
gateway WSS port 9000, SSH laptop forward 19000, transparent LAN relay 9000 and
Unity Android. The phone pinned the gateway certificate; the laptop did not
terminate phone TLS. Sensor port 5000 and gateway ports 9000/9100 were observed
listening concurrently on Ultra96.

| Check | Recorded result | Interpretation |
| --- | --- | --- |
| Desktop and physical phone | Deterministic 10 Hz source, separate publisher and keyboard/FireBeetle pipeline reached Unity/Android. Both physical hubs completed the path. | Communications demonstration with dummy/random labels, not model inference. |
| Correlated final event | Hub 6 request sequence 17 produced visualizer sequence 24, exercise squat; phone displayed the matching exercise/sequence. | One correlated physical display observation; see handoff for run/session identifiers. |
| Replay and run changes | Duplicate response produced no second event; conflicting request reuse produced none; repeated phone sequence rejected, new run accepted after restart. | Physical examples plus automated malformed/unauthorized/stale-session coverage. |
| Phone stopped | Both sensor streams continued at approximately 50 packets/s each with zero reported integrity counters. | Demonstrated separation of phone availability and ingestion. |
| Phone reconnect | Gateway retained the newest update with no visualizer connected and delivered it on reconnect. | Latest state delivery, not retention of every intermediate classification. |
| Gateway stopped | Sequence 10 publication was dropped with connection-refused logging; sensors stayed healthy. After restart, Unity received sequence 11. | Failed publication was not retried by the producer. |
| TLS startup and client limit | Missing/invalid TLS configuration failed closed; a second simultaneous visualizer was rejected manually. | Recorded manual checks. |
| Certificate interoperability | Built-in Unity ClientWebSocket rejected the self-signed certificate before the callback; the pinned SslStream/WebSocket transport worked on the physical phone. | Identifies the integration issue and tested solution. |
| September automated snapshot | 30 Ultra96/protocol/gateway tests, 17 laptop tests and 6 Unity EditMode protocol tests were recorded as passing on/by 29 September. | Historical counts, superseding the Phase 10 development snapshot of 26 Ultra96 tests; not a new test run. |

The original one-day gateway certificate expired and was replaced with a 90-day
certificate valid 28 September through 27 December 2026, with IP SAN 192.168.0.2.
Unity was rebuilt with its public certificate. The recorded project key/certificate
location was temporary `/tmp/comms/tls-project`, distinct from earlier one-day
setup examples under `/tmp/comms/tls`; confirm actual deployed material before use.

The phone was provisioned as 192.168.0.200, outside DHCP range .100-.199, with
Use phone MAC enabled. The recorded firewall rule restricted the phone path
by remote IP, laptop local IP, TCP port, executable and Public profile. Windows
reported interface type Any; local-address restriction selected the private path.

The visualizer plan still called for a recorded five-minute rehearsal and
physical stale/reconnect sequence. Later records prove a matching phone display
and some reconnect cases, but do not establish that complete timed acceptance
sequence. The 500 ms stale display after an isolated event was observed; that
is a configured freshness threshold, not measured inference latency. Required
setup/encryption/concurrency videos were outstanding in the September snapshot;
completion cannot be inferred here. The home-directory ownership issue and
persistence/reboot procedure require current deployment verification in
[Ultra96 setup notes](../ultra96/ULTRA96_SETUP_NOTES.md).

## 6. Feedback, idle and fall checkpoints

These checkpoints describe intermediate revisions. Their old rep flashes, ready
patterns and idle beeps are historical; final outputs are documented in
[Design evolution](DESIGN_EVOLUTION.md#7-phone-visualizer-and-feedback-evolution).

| Checkpoint | Recorded verification | Physical/integration limit |
| --- | --- | --- |
| [Stage 1](../firmware/FireBeetle_to_Ultra96/test/protocol_host/STAGE1_CHECKPOINT.md) | Seven host executables passed; new profiles built with 76,544 bytes static RAM and approximately 866 kB firmware. Local output behavior was later user-confirmed on 1 November. | User confirmation is distinct from host timing/GPIO tests; no measured on-board jitter distribution. |
| [Stage 2](../firmware/FireBeetle_to_Ultra96/test/protocol_host/STAGE2_CHECKPOINT.md) | Feedback codecs/replay/retry and two production-C peer integration; both profiles built, RAM 77,864 bytes and firmware 867,339/867,343 bytes. User subsequently confirmed commands. | Lost-ACK injection primarily automated; an ACK establishes firmware handling, not observed sound/light. |
| [Stage 3](../firmware/FireBeetle_to_Ultra96/test/protocol_host/STAGE3_CHECKPOINT.md) | User-confirmed simulator cases: duplicate counts, count jumps, reconnect synchronization, new sessions and an absent node. Managed protocol/real C# WSS checks passed. | Actual teammate AR counter/updated APK remained untested at that checkpoint; Unity batch Editor execution was license-blocked. |
| [Stage 4](../firmware/FireBeetle_to_Ultra96/test/protocol_host/STAGE4_CHECKPOINT.md) | 15 Stage 4 integration tests passed. User confirmed two idle episodes, Stage 3 regression and manual STOP/disconnect behavior. | Host inference tests fed deterministic results through supplied pipeline logic; did not run DPU models. Earlier C# TLS tests required an outside-sandbox rerun. |
| [Reduced Stage 5](../firmware/FireBeetle_to_Ultra96/test/protocol_host/STAGE5_CHECKPOINT.md) | 11 Stage 5 Python/C-peer integration tests; both profiles built, RAM 78,480 bytes and firmware 870,179/870,183 bytes. Tests covered detector inputs, alarm priority, queue bounds, retries and receipt deduplication. | Physical verification pending there; AR forwarding intentionally deferred at that time. Later implementation does not retroactively complete that physical checkpoint. |

Detailed stage records retain source/flash archive paths and SHA-256 manifests.
Old profile names refer to those saved revisions. Rebuilding an old profile
against new shared source would not reproduce its original behavior; current
PlatformIO configuration only retains final and diagnostic environments.

The final implementation includes the restored detector, boot-default ON,
acknowledged server settings and phone fall receipts. Deployed firmware/APK
and live IMU/phone acceptance are still required. Synthetic injection validates alarm
and reporting; it cannot establish detector sensitivity or false-positive rate.
Do not transfer Stage 5's older gap/expiry rules to the restored final algorithm.

## 7. Physical integration and dynamic-window verification

The [hardware vertical slice](handoffs/HARDWARE_COMMUNICATIONS_VERTICAL_SLICE.md)
records host golden vectors, NaN/Inf rejection, chronological history-to-live
delivery, overwritten-history reporting, two simulated sources, replay/session
checks and firmware builds before physical MPU6050 verification. Intermediate
exercise/Bluetooth builds and their memory figures belong to that revision.

The [dynamic-node guide](../ultra96/ONE_NODE_MODE.md) records 45 tests after removal
of legacy capture mode, and an earlier portable selection of 228 passing checks
with documented reruns/environment exclusions. It explicitly does not claim a
current all-green full discovery. Physical flashing, either-node startup,
joining/loss recovery with real sensors and comparative inference accuracy
remain separate acceptance work.

A historical integrated-server checkpoint recorded 14 integration tests covering
independent startup, late joining, node-loss fallback, stale-session rejection,
STOP/rearming, real-only windows and startup wiring. Its feedback-ACK receiver
fix replaced a call to the removed legacy coordinator method
`super().control_acknowledged` with direct dispatch to
`FeedbackDelivery.acknowledge`. The old call raised an `AttributeError` on a
feedback ACK, terminated the receiving worker and caused a reconnect during
capture. Regression checks covered activity, duplicate, partner-node, rep and
malformed ACKs. Startup/model runners were test doubles; this checkpoint did not
program the DPU or validate physical inference.

The earlier
simulated single-node model outputs remain evidence of routing/control, not
measurements of that updated model's accuracy. Use the matching updated runtime;
the [server guide](../ultra96/README.md#choose-one-sensor-server) records the older
local-copy version difference. Final source defaults of 100 Hz, 300 nominal samples per window,
one-second hops, 701 history entries and a 4% integrated missing allowance are
configuration facts. Two physical 100 Hz producers using 50-byte frames imply
10,000 application bytes/s before control/transport overhead; this is arithmetic,
not a fresh throughput result. No battery-life measurement was retained in the
consolidated source notes.
