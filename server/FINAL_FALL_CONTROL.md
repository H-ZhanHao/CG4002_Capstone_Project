# Final fall-detection integration

The AR enable/disable feature was removed on 2026-11-13. Hip fall detection
runs automatically in `node1_final`; AR receives and acknowledges fall events
and displays the existing centred popup. Calibration and detector thresholds,
local alarm behaviour, rep feedback, and the Unity assembly-reference fixes
are retained.

## Firmware

`node1_final` and `node2_final` remain available. Both set `APP_FALL_DETECTION=1`
and use the existing 100 Hz capture, 1 ms tick and independent-node TLS paths.
The detector remains Hip-only. There is no runtime enable flag, AR setting
command or default-OFF dependency. Phone disconnects/reconnects have no effect
on detection. All 36 earlier PlatformIO sections retain their settings;
`node*_update_fall2` still disables detection at build time.

From `communications/firmware/FireBeetle_to_Ultra96`:

```powershell
pio run -e node1_final -e node2_final
pio run -e node1_final -t upload --upload-port <HIP_COM_PORT>
pio run -e node2_final -t upload --upload-port <WRIST_COM_PORT>
```

## Update Ultra96

Replace these beside the running `server_rep_fall.py`:

- `server/dynamic_capture.py`
- `server/rep_control.py`
- `server/visualizer_protocol.py`

Replace `communications/ultra96/visualizer_gateway.py` in the gateway directory,
and its `visualizer_protocol.py` if the gateway runs from a separate directory.
Restart both services. `fall_control.py` is no longer used and has been removed
from the repository. The supplied `server_rep_fall.py` and
`window_assembler_updated.py` remain unchanged by this removal.

## Update Unity / AR handoff

Replace `Ultra96VisualizerClient.cs` and `FallProtocol.cs` in the existing
Networking folder. Update `ExerciseDebugPanel.cs` if using the demo display;
the button has been removed and the working centred fall popup is retained.
Keep the corrected runtime/test `.asmdef` files and `VisualizerBootstrap.cs`.
Rebuild/install the APK to apply the change. Previously shared runtime/demo
ZIP packages in `.build/ar_fall_handoff/` have been refreshed.

The teammate's remaining integration API is:

- Subscribe to `client.FallReceived` to display their AR popup. Callbacks run
  on Unity's main thread; unsubscribe when the UI is disabled.
- Read `client.FallAlertVisible` when restoring a UI after scene changes.
- Read `client.LatestFall` for the event identity.
- Call `client.DismissFallAlert()` when dismissing the popup.

The client sends the receipt ACK automatically and suppresses duplicate alerts.
It no longer exposes enable/disable methods or sends a setting on reconnect.
There should be one persistent networking client per app.

Fall notification JSON is unchanged:

```json
{"v":1,"type":"fall_event","node_id":1,"boot_id":1234,"event_id":"1"}
```

Its receipt uses the same identity with `type: "fall_ack"`. `event_id` remains
a string. Existing exercise and rep messages are unchanged.

## Validation

From the repository root, with the existing Python gateway requirements:

```powershell
python -B -m unittest discover -s server -p test_one_node_integration.py
python -B -m unittest discover -s server -p test_fall_notifications.py
& communications/firmware/FireBeetle_to_Ultra96/fall_tests/run_tests.ps1
& communications/unity/tests/run_protocol_host.ps1
```

The notification tests use a real loopback TLS gateway, the supplied server
and simulated ESP events. They check delivery without an enable request or
rep session, reconnect replay, receipt acknowledgement, and rejection of the
removed setting command. OpenSSL is required for the TLS test.
Physical firmware flashing, APK installation and live IMU/phone acceptance
must be repeated on the deployed devices.
