# ESP32 Individual Demo: Phase 10 Implementation Handoff

> **Historical test record - 28 September 2026, Phase 10.**
> Physical Hub 3/6, Ultra96, Unity Editor and Android exercised the keyboard-to-
> phone communications demonstration while approximately 50 Hz mock streams ran.
> Exercise labels were simulated; the recorded PASS does not establish real AI
> accuracy, final feedback/fall acceptance or final physical acquisition timing.
> Commands, certificate dates and temporary deployment locations describe this
> revision and must not be assumed to match the current deployment. See
> [current setup references](../README.md#component-guides) and
> [recorded phone results and limits](../EXPERIMENTS_AND_RESULTS.md#5-phone-visualizer-and-isolated-failures).

This work prepared for the Individual Subsystem Test on 1 October; it does
not describe final-product integration.

Date recorded: 28 September 2026  
Physical status: **PASS on FireBeetle Hubs 3 and 6, Ultra96, Unity Editor, and
the Android phone**

## Implemented path

```text
laptop ordinary keystroke
  -> random complete binary IMU TEST_REQUEST to selected/random ready hub
  -> FireBeetle TEST_RESPONSE
  -> Ultra96 full response validation and Session-scoped replay check
  -> one random required-field-only exercise_update
  -> bounded background TCP publication to 127.0.0.1:9100
  -> existing loopback WSS gateway and phone path
```

The laptop log now includes Hub, Session, request Sequence, editable payload
index, accelerometer and gyroscope tuples, and readable
status names. Existing selection, duplicate, conflict,
benchmark, file-transfer, and shutdown keys are unchanged.

The interactive laptop entry point is `laptop/interactive_hub_relay.py`; its
automated tests are in `laptop/test_interactive_hub_relay.py`. The independent
opaque forwarding utility remains `laptop/tcp_relay.py`.

## Ultra96 behavior

- `TEST_RESPONSE` is fully checked for active Session, authenticated Hub Node
  ID, flags, payload length, canonical IMU decoding, and required mock/processed
  status bits before any event is created.
- Each connection owns a replay window capped at 256 response payloads. An exact
  duplicate is ignored, a conflicting or stale Sequence is rejected, and the
  tracker disappears when the connection/Session ends.
- The publisher is shared by Hub 3 and Hub 6. A lock assigns unique increasing
  `seq` values across concurrent workers.
- `run_id` is a freshly generated eight-digit process-run identifier. It changes
  when the server publisher restarts, while `seq` starts at zero and increases
  within that run.
- Until real inference is connected, exercise selection is random among
  `unknown`, `squat`, `push_up`, `sit_up`, `lunge`, and `leg_raise`.
- Events use `visualizer_protocol.build_message` and contain the six version-1
  fields.
- A bounded 64-entry queue moves loopback connection and write work off the hub
  threads. Connection/write timeouts are 250 ms. Failure or queue saturation is
  logged as one compact `VISUALIZER_DROP` line and cannot close a hub Session.
- `server.py` writes the decoded operator output to both the coloured terminal
  and `/tmp/comms/logs/server.log`. The plain-text file rotates at 10 MiB and
  retains five older segments; path, size, and retention are command-line
  configurable. Accepted `FILE_CHUNK` records include offset, length, CRC, and
  `crc_valid=true`.
- Port 9100 remains the existing gateway-owned loopback listener. No new
  externally bound listener, tunnel, relay, Unity format, or firmware behavior
  was introduced.

## Automated verification

Results on 28 September 2026:

```text
laptop full unittest discovery: 17 passed
Ultra96 full unittest discovery: 26 passed
Python compilation of affected modules: PASS
git diff --check: PASS (line-ending warnings only)
```

Coverage includes valid publication, required fields, exact duplicate
suppression, conflict rejection,
malformed/unauthorized/stale-Session rejection, tracker
reset on a new Session, concurrent Hub 3/6 sequencing, publisher unavailability,
and existing sensor/file behavior.

The complete Ultra96 discovery used the repository virtual environment with
its pinned `websockets==12.0` dependency. Gateway coverage includes phone
disconnect/reconnect, latest-only delivery, malformed publisher input, and
loopback TLS.

No ESP32 source changed, so no firmware profile was rebuilt.

## Physical verification

The complete path was exercised on 28 September 2026 with both physical hubs,
the Ultra96 services, the SSH tunnel, the laptop relays, Unity Editor, and a
rebuilt Android APK.

### Final correlated phone event

The final automatically selected request used Hub 6:

```text
TEST_REQUEST hub=6 session=3670016966 sequence=17 payload_index=0
  accel=(100, -200, 1000) gyro=(10, -20, 30)
  status=mock_data

TEST_RESPONSE hub=6 session=3670016966 sequence=17
  accel=(106, -200, 1000) gyro=(110, -20, 30)
  status=mock_data|test_request_processed

VISUALIZER_EVENT hub=6 session=3670016966 request_sequence=17
  visualizer_run_id=8692590753886131623
  visualizer_sequence=24 exercise=squat

VISUALIZER sent run_id=8692590753886131623 seq=24 exercise=squat
```

The Android phone displayed `squat`, the matching run suffix and Sequence 24.
It transitioned from `LIVE` to `STALE` after 500 ms as designed for a single
event. The current panel displays `Run ID:` and `Sequence:` on separate lines.

### Hub 3 and Hub 6

Hub 3 and Hub 6 both completed the pipeline. Representative transformed values
were `ax 100 -> 103` on Hub 3 and `ax 100 -> 106` on Hub 6, with `gx 10 -> 110`
on both. A maximum-value Hub 6 payload also proved saturation from `gx 32700`
to `32767`. The phone displayed matching monotonically increasing visualizer
Sequences from the shared run.

### Exactly once and rejection behavior

- An exact duplicate Hub 3 response for Session `3881472074`, request Sequence
  3 was received by Ultra96 but produced no second `VISUALIZER_EVENT` or `VISUALIZER
  sent` line.
- A conflicting reuse of that request Sequence returned Hub error code 8 and
  likewise produced no visualizer event.
- Automated tests cover malformed, unauthorized, stale-Session, and additional
  conflicting responses; none publish an event.

### Failure isolation

- Stopping Unity closed the gateway client while both hubs continued at about
  50 packets/s each, roughly 100 packets/s combined, with zero gaps,
  duplicates, out-of-order frames, or timestamp regressions.
- An event generated with no visualizer connected was retained by the gateway
  and delivered immediately when Unity reconnected.
- With the gateway stopped, Ultra96 generated visualizer Sequence 10, logged
  `VISUALIZER_DROP ... reason=ConnectionRefusedError`, and kept both sensor
  Sessions and streams healthy. After gateway restart, Unity reconnected and
  received the next event at Sequence 11; the failed event was not retried.

### TLS and phone provisioning

The original one-day visualizer certificate had expired. It was replaced with
a 90-day self-signed project certificate valid from 28 September through 27
December 2026 with `IP:192.168.0.2` in its SAN and a mode-600 private key kept
only on the Ultra96. Unity was rebuilt with the matching public certificate.

The Ultra96 home directory remains unwritable, so this certificate currently
resides under `/tmp/comms/tls-project` and may be lost on reboot. Moving the
private key to protected persistent Ultra96 storage remains an operational
follow-up, not a Phase 10 protocol defect.

The phone uses static private-router address `192.168.0.200`, outside the
router's DHCP pool of `192.168.0.100` through `192.168.0.199`, and keeps `Use
phone MAC` enabled for this network. A Windows Firewall inbound allow rule
named `CG4002 Visualizer Phone` permits only that remote address, laptop address
`192.168.0.2`, TCP port 9000, the exact Python 3.14 relay executable, and the
Public profile. A 29 September audit confirmed that Windows reports the rule's
interface type as `Any`; the local-address restriction selects the `Wi-Fi 2`
path. The phone successfully reconnected after the static-address and
firewall-rule update.

## Phase 10 acceptance summary

```text
Random complete binary payload selection and readable laptop log: PASS
Automatic and explicit Hub 3/6 selection: PASS
Correlated transformed TEST_RESPONSE: PASS
Exactly one required-field-only exercise_update per new response: PASS
Six-field event format: PASS
Exact duplicate produces no second event: PASS
Conflicting replay produces no event: PASS
Thread-safe monotonically increasing visualizer Sequence: PASS
Gateway/phone disconnect does not stop sensor reception: PASS
Gateway unavailable is logged and isolated: PASS
Pinned WSS through SSH and laptop relay: PASS
Physical Android phone displays matching event: PASS
Both normal 50 Hz streams remain healthy: PASS
```

Phase 10 is complete and physically verified. The temporary Ultra96
certificate location remains a deployment-maintenance item.
