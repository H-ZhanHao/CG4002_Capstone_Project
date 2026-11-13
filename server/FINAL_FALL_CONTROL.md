# AR fall-detection control

Implemented on 2026-11-13 using the supplied `server_rep_fall.py`,
`window_assembler_updated.py`, and `dynamic_capture.py`. The server and window
assembler are unchanged from those supplied files. The capture coordinator adds
the fall-control handler and a compatibility fallback for older server feedback
ACK handling. Matching shared helpers are present in `server/` and
`communications/ultra96/`.

## Behaviour

- `node1_final` and `node2_final` are new PlatformIO environments. All earlier
  environment definitions are unchanged, including the disabled-fall
  `node*_update_fall2` profiles.
- The final profiles compile in fall detection but start it OFF. Only Hip /
  Node 1 accepts the runtime setting. Legacy profiles retain their existing
  startup behaviour and reject this setting command.
- The Unity debug panel exposes Enable/Disable. After an unconfirmed result it
  offers explicit Set ON / Set OFF buttons. The same public client API can be
  bound to a button in an AR scene:
  `SetFallDetectionEnabled(bool enabled)`.
- A setting remains pending until the ESP sampling task applies it and returns
  an ACK. A successful API send alone does not mean the ESP changed its setting.
- One setting command is outstanding at a time. The server waits two seconds
  for its ACK; Unity has a four-second overall response timeout. There are no
  automatic setting retries or separate state-query messages. Retry manually.
- Every AR control connection requests OFF when its welcome message arrives.
  OFF is displayed as confirmed only after the ESP ACK. Reconnecting while Hip
  is unavailable leaves the setting unconfirmed; nothing is queued for replay.
- Losing the phone connection retains the ESP's last applied setting. ESP
  reboot resets it OFF. Replacing/losing Hip's server connection invalidates an
  existing displayed confirmation; the user can explicitly set ON or OFF again.
- Changing mode resets partial fall confirmation. Existing alarms finish and
  queued fall reports remain deliverable. Sensor capture, exercise processing,
  and rep feedback continue independently. Synthetic `fall inject` also respects
  OFF; the separate buzzer diagnostic is still an output test.

## Deployment

Deploy these together beside the existing AI server dependencies:

- `server/server_rep_fall.py`
- `server/window_assembler_updated.py`
- `server/dynamic_capture.py`
- `server/fall_control.py`
- `server/rep_control.py`
- `server/visualizer_protocol.py`

Update the running gateway with
`communications/ultra96/visualizer_gateway.py` and the matching
`visualizer_protocol.py`. Retain its existing fall-delivery and TLS dependencies.
No ports, relay routes, certificates, or AI model changes are needed. Launch the
AI server using its existing `python server_rep_fall.py` command.

Build and flash from `communications/firmware/FireBeetle_to_Ultra96`:

```powershell
pio run -e node1_final -e node2_final
pio run -e node1_final -t upload --upload-port <HIP_COM_PORT>
pio run -e node2_final -t upload --upload-port <WRIST_COM_PORT>
```

Each new environment gets its own `.pio/build/node*_final` output and generated
`sdkconfig.node*_final`. They inherit the cleanup profile's 100 Hz capture,
1 ms scheduler tick, independent-node TLS transport, and feedback behaviour.

Rebuild the Unity project/APK to include the controls. The checked-in APK has
not been rebuilt by this implementation.

## Protocol

AR sends bounded JSON through the existing WSS / loopback control connection:

```json
{"v":1,"type":"fall_detection_set","run_id":123,"seq":1,"enabled":true}
```

The result has type `fall_detection_result`, the same `run_id` and `seq`,
`requested_enabled`, `status`, `reason`, and `enabled`. `enabled` is a boolean
only for `status: "applied"`; otherwise it is null. Other statuses are
`pending`, `rejected`, `timeout`, and `unavailable`.

Ultra96 sends binary command `0x0C` to Hip using the existing header/session and
a four-byte payload: boolean 0/1 followed by three zero reserved bytes (`!B3x`).
ESP replies using the existing `CONTROL_ACK` (`0x09`), with command type `0x0C`
and the matching command sequence. Wrong-node, stale-session/sequence, late,
and superseded ACKs cannot confirm a newer request.

## Validation

From the repository root, in a Python environment with the existing gateway
requirements installed (`communications/ultra96/requirements.txt`):

```powershell
python -B -m unittest discover -s communications/ultra96 -p test_final_fall_control.py
python -B -m unittest discover -s server -p test_one_node_integration.py
& communications/firmware/FireBeetle_to_Ultra96/fall_tests/run_tests.ps1
& communications/unity/tests/run_protocol_host.ps1
```

The WSS tests use a simulated ESP connection, the real supplied server
coordinator/control service, the real gateway, and a temporary trusted TLS
certificate. OpenSSL is required for that test. The managed host compiles the
Unity networking/UI source and executes protocol cases without launching Unity.
Both final firmware environments have been built successfully. Physical IMU,
phone UI, flashing, and live Ultra96/DPU acceptance remain hardware checks.

The legacy `test_server` and `test_window_assembler` suites currently report 24
failures and 9 errors across 96 tests. The same failing cases occur with the
original Git capture coordinator, so they predate this feature. The supplied
server's 14 active integration tests and the feedback-delivery regression suite
pass with the updated coordinator.
