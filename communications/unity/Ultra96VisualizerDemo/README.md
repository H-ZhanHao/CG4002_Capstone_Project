# Ultra96 Unity visualizer

The project uses Unity 6000.6.2f1 as recorded in `ProjectSettings/ProjectVersion.txt`. The bootstrap
creates/reuses one networking client after scene load and attaches the debug
panel. Normal phone URL is `wss://192.168.0.2:9000`.

Follow [Network setup](../../docs/NETWORK_SETUP.md) for the private phone IP,
firewall, SSH and relays, and [Ultra96 services](../../ultra96/README.md) for the
publisher/gateway. Do not create another client for each AR UI or scene.

## Supply the pinned public certificate

Obtain the **public certificate actually passed to the running gateway's
`--tls-cert` argument**. Its location is a deployment choice, not a fixed `/tmp`
path. Verify IP SAN `192.168.0.2` and validity dates using OpenSSL. Keep the
Ultra96 private key on Ultra96; the laptop wearable relay's certificate is a
different identity and is not the phone's trust anchor.

From the repository root on the Unity machine, using the actual public-file path:

```powershell
Copy-Item -LiteralPath 'C:\path\to\gateway-server-cert.pem' `
  -Destination 'communications/unity/Ultra96VisualizerDemo/Assets/Resources/Certificates/server-cert.bytes'
```

The file remains PEM text; `.bytes` imports as a Unity TextAsset. The client
loads `Certificates/server-cert`, enforces the exact SHA-256 certificate pin
and checks certificate dates. SAN is verified during provisioning. Missing,
expired or mismatched resources fail closed. Changing the gateway certificate
requires a matching resource and rebuilt app. Both PEM and `.bytes` files are
ignored by Git and must be deliberately provisioned on each build machine.

## Editor and physical Android checks

For an isolated exercise-display test, keep these running:

1. Ultra96 gateway with its valid TLS files and `--demo-source`.
2. Laptop SSH forward `127.0.0.1:19000` to Ultra96 `127.0.0.1:9000`.
3. Transparent phone relay `192.168.0.2:9000` to laptop `127.0.0.1:19000`.

Open this Unity project, open `Assets/Scenes/SampleScene.unity` and press Play.
The panel should display LIVE with run, sequence, timestamp and cycling demo
labels. Those labels are test output, not real inference. Stop/restart the phone
relay to observe stale/disconnected/reconnected state. The client marks an
exercise update stale after 500 ms and reconnects with bounded 1, 2, 4, then
5 s delays. Fall handling is independent of exercise freshness.

For Android, install the project's Android build support, join the phone to
the private 2.4 GHz LAN, provision the recorded `.200` address/Use phone MAC
setting and confirm the port-9000 firewall path. Build/install the current APK;
Android Internet permission is enabled in the project. Compare a displayed
run/sequence with the gateway log and repeat reconnect testing on the device.
For real operation omit gateway `--demo-source` and start the selected sensor/
inference publisher; two result sources should not compete during acceptance.

## Rep counting integration

AR owns counting. Calls are on Unity's main thread and use the existing WSS
connection. The checked-in API is:

- `BeginRepSession(exercise, baselineCount = 0)` starts/resets a set explicitly.
- `SyncRepSession(currentCount)` synchronizes an existing set on reconnect.
- `SendRepEvent(repIndex)` supplies the cumulative count for a new rep.
- `RepSessionReady` and `RepFeedbackReceived` expose readiness and receipts.

Wait for session readiness/receipts before sending new reps. `Task<bool>` send
success means written to WSS, not that firmware applied a beep. Busy/disconnected
rep sends are not queued; synchronize the current count after reconnect rather
than replaying missed reps. A count jump yields one current Hip beep, duplicate
counts yield none, and resetting count requires a new rep session. Wrist has no
rep beep and activity LEDs do not flash per rep. The debug panel is not a rep
counting algorithm or the teammate's finished AR application.

## Fall notifications

The client accepts exactly five `fall_event` fields: version, type, Hip node ID,
boot ID and positive decimal-string event ID. It automatically sends a matching
`fall_ack` after main-thread acceptance. Subscribe/unsubscribe to `FallReceived`
with UI lifetime; `LatestFall`, `FallAlertVisible` and `DismissFallAlert()` let
AR present/dismiss its popup. The debug display uses a centred red FALL DETECTED
popup and Dismiss alert button. Dismissal is local; receipt ACK does not mean a
person saw the alert.

Duplicate/retried events are ACKed without raising the callback or redisplaying
a dismissed alert. Deduplication survives reconnect for this component lifetime,
with up to 64 boot identities/high-water marks; capacity exhaustion fails closed
until client restart. It is not durable across app restart. Only one persistent
network client should own these events.

The phone has no detector ON/OFF command or toggle. Hip starts ON with final
firmware and its server terminal controls the setting.
USB `fall inject` is marked synthetic and filtered from phone alerts;
`feedback fall-alarm` is sound-only. Neither proves real detector-to-phone
acceptance. For the full path, produce a qualified detector event using a
controlled sensor fixture and compare DEVICE_EVENT_ACCEPTED, FALL_AR_QUEUED,
FALL_AR_RECEIVED and FALL_AR_ACKED; check stopped capture and a later distinct event.

## Tests

With a licensed Editor and installed test packages, open Window > General >
Test Runner, choose EditMode and run the supplied ExerciseUpdateProtocolTests,
RepFeedbackProtocolTests, RepSessionSendTests and FallProtocolTests. Verify that
the test source files are present; some test resources remain Git-ignored.

The optional managed protocol helper, from the repository root:

```powershell
& ./communications/unity/tests/run_protocol_host.ps1
```

It requires .NET and generated `CG4002.Visualizer.csproj`/
`CG4002.Visualizer.Tests.csproj` files with valid installed Unity/NUnit/Newtonsoft
references. Open/regenerate the Unity project files first if those ignored files
are absent. Managed checks do not exercise actual scene lifecycle, Android TLS,
AR counting or physical sensor/GPIO behavior. Historical passes/license limits
and pending device evidence are in [Experiments and results](../../docs/EXPERIMENTS_AND_RESULTS.md).
