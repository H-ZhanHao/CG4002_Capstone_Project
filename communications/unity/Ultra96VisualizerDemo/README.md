# Ultra96 Unity visualizer demo

This project starts a minimal WSS visualizer automatically when any scene is
played. No scene object needs to be configured. The on-screen panel shows the
connection state, exercise, run ID, sequence, and Ultra96 timestamp.

## Supply the pinned public certificate

Keep the Ultra96 private key on the Ultra96. Copy only its public certificate
into the ignored Unity resource path:

```powershell
cd D:\NUS\CG4002-communications

Copy-Item `
  "$env:TEMP\cg4002-ultra96-cert\server-cert.pem" `
  "unity\Ultra96VisualizerDemo\Assets\Resources\Certificates\server-cert.bytes"
```

The certificate must be the one currently presented by the Ultra96 gateway and
must contain `IP:192.168.0.2` in its Subject Alternative Name. Provisioning
verifies that SAN, while the client enforces the exact SHA-256 certificate pin
and validity dates. Exact pinning is used because Unity TLS backends differ in
their handling of IP-address SANs. If the resource is absent, expired, invalid,
or different from the presented certificate, the panel reports an error and
does not connect. There is no trust-all fallback.

## Desktop Editor smoke test

Keep these three processes running:

1. Ultra96 WSS gateway in `--demo-source` mode.
2. Laptop SSH forward from `127.0.0.1:19000` to Ultra96 `127.0.0.1:9000`.
3. Laptop transparent relay from `192.168.0.2:9000` to `127.0.0.1:19000`.

Open this project in Unity 6, open `Assets/Scenes/SampleScene.unity`, and press
Play. The panel should become `LIVE` and cycle through `unknown`, `squat`,
`push_up`, `sit_up`, `lunge`, and `leg_raise`. Stop the relay to verify `STALE`
followed by `DISCONNECTED`; restart it to verify bounded automatic reconnection.

## Physical Android phone test

1. Connect the phone to the DIR-842 private Wi-Fi.
2. Keep the laptop `Wi-Fi 2` interface at `192.168.0.2/24`. Ethernet
   `192.168.0.3/24` is only a backup private-router connection.
3. Keep the phone at reserved address `192.168.0.200` with **Use phone MAC**
   enabled. The validated `CG4002 Visualizer Phone` firewall rule restricts
   TCP 9000 to that phone, local address `192.168.0.2`, the exact Python 3.14
   relay executable, and the Public profile. Windows reports its interface
   type as `Any`; the local-address restriction selects the `Wi-Fi 2` path.
4. Select the existing Android build profile and build/install the APK.
5. Launch it and confirm the panel becomes `LIVE` at
   `wss://192.168.0.2:9000`.
6. Compare the phone's run and sequence against the Ultra96 gateway log.
7. Stop and restart the relay to check stale, disconnect, and reconnect states.

Android Internet permission is explicitly enabled in `ProjectSettings.asset`.

## Tests

In Unity, open **Window > General > Test Runner**, select **EditMode**, and run
all tests. `ExerciseUpdateProtocolTests` verifies required fields and covers
malformed JSON, unsupported versions/types, and invalid ranges.

## Fall notifications

The client accepts the five-field `fall_event` JSON over its existing pinned
WSS connection and sends `fall_ack` with the same node/boot/event identity after
main-thread acceptance. It exposes `FallReceived`, `LatestFall`,
`FallAlertVisible`, and `DismissFallAlert()` for AR integration. These work before
any rep session starts and independently of exercise update freshness.

The existing demo debug panel displays a red **FALL DETECTED** banner, boot/event
identity and a Dismiss alert button. Dismissal is a local UI operation; receipt
ACK is automatic. Duplicate/retried events are ACKed without raising the callback
again or redisplaying a dismissed alert. Deduplication survives WSS reconnects
for the client component lifetime. Up to 64 boot identities are retained, with
event high-water marks; reaching capacity fails closed and logs a warning until
the client is restarted. This assumes increasing event order from the existing
Ultra96 receiver. Rebuilding/installing this client is necessary; existing APKs
do not understand fall notifications.

For a physical check, update the Ultra96 files listed in its README, rebuild the
demo, connect normally, and use a safe sensor test fixture to produce a real
detector event. Expect `DEVICE_EVENT_ACCEPTED`, `FALL_AR_QUEUED`, Unity's
`FALL_AR_RECEIVED`, the visible banner, and `FALL_AR_ACKED`. Confirm delivery
while capture is stopped, dismiss the alert, and verify a distinct later event
shows another alert. USB `fall inject` intentionally remains local/server-only;
do not use it as proof of AR delivery. Automated tests provide controlled fall
frames without requiring a person to fall.
