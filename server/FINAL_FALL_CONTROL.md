# Final fall-detection integration

Fall detection is controlled from the server terminal. Hip starts ON in
`node1_final`; AR receives and acknowledges fall events
and displays the existing centred popup. The fall algorithm and thresholds
are restored from `cfcb6ac`; exercise calibration, local alarm duration, rep
feedback, and the Unity assembly-reference fixes are retained.

## Firmware

`node1_final` and `node2_final` remain available. Both set `APP_FALL_DETECTION=1`
and use the existing 100 Hz capture, 1 ms tick and independent-node TLS paths.
The final profiles also set `APP_FALL_RUNTIME_CONTROL=1`: the detector starts
ON on each ESP reboot, and only Hip accepts the server's runtime command.
Phone and server disconnects/reconnects retain the ESP's current setting.
The Wrist adapter does not detect or report falls. Setting
`APP_FALL_DETECTION=0` still disables detection at build time.

The restored detector requires acceleration magnitude below 4 m/s^2 for at
least 100 ms before waiting for an impact above 25 m/s^2. It reports immediately
on qualifying impact, without posture/gyro confirmation. The nominal impact
wait is 1000 ms; the historical impact-before-expiry check is preserved.
The communications adapter retains its 100 ms gap limit, invalid-input and
timestamp resets, runtime setting reset, Hip-only reporting and alarm cooldown.

For this detector rollback alone, rebuild and flash the final firmware onto
the boards. No Ultra96 files or AR app need replacing if the server controls
documented below are already deployed.

## Server commands

Type into the terminal running `python server_rep_fall.py` (the retained
`communications/ultra96/server.py` console supports the same commands):

```text
fall on
fall off
fall status
```

`fall on` / `fall off` send an explicit setting to Hip using binary command
`0x0C`, with payload `!B3x` (0/1 followed by three reserved zero bytes).
Success is logged only after Hip's sampling task applies it and returns the
matching `CONTROL_ACK` (`0x09`). Capture and rep feedback remain independent.
Disabling clears partial detection; existing alarms and queued reports finish
normally. The diagnostic `fall inject` also respects OFF.

`fall status` reports the last confirmed setting on the current connection.
It reports unknown before a setting is acknowledged, after timeout, or after
Hip disconnects/reconnects; it does not query the device or infer OFF merely
from a connection. Reissue `fall on` or `fall off` to confirm a setting.
There is one outstanding setting at a time, a two-second ACK deadline, and
no automatic retries or replay when an offline Hip reconnects. An unavailable
Hip, unsupported legacy firmware, or a lost ACK cannot report false success.
AR has no setting command and never sends OFF on reconnect.

From `communications/firmware/FireBeetle_to_Ultra96`:

```powershell
pio run -e node1_final -e node2_final
pio run -e node1_final -t upload --upload-port <HIP_COM_PORT>
pio run -e node2_final -t upload --upload-port <WRIST_COM_PORT>
```

## Update Ultra96

Replace these beside the running `server_rep_fall.py`:

- `server/server_rep_fall.py`
- `server/dynamic_capture.py`
- `server/fall_control.py`

Flash the rebuilt `node1_final` onto Hip to apply the default-ON behavior;
the boot-default change needs no server update. For the retained communications
server's terminal controls, deploy its matching
`server.py`, `dynamic_capture.py` and `fall_control.py`. The two helper copies
are identical. No gateway, phone protocol, model, or window assembler changes
are needed for terminal control.

## Update Unity / AR handoff

The AR toggle remains removed. When updating from the older toggle version,
replace `Ultra96VisualizerClient.cs` and `FallProtocol.cs` in the existing
Networking folder. Update `ExerciseDebugPanel.cs` if using the demo display;
the button has been removed and the working centred fall popup is retained.
Keep the corrected runtime/test `.asmdef` files and `VisualizerBootstrap.cs`.
Rebuild/install the APK to remove the old controls. An app already running the
alert-only client needs no update for these server terminal commands.

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
python -B -m unittest discover -s server -p test_server_fall_control.py
python -B -m unittest discover -s server -p test_fall_notifications.py
& communications/firmware/FireBeetle_to_Ultra96/fall_tests/run_tests.ps1
& communications/unity/tests/run_protocol_host.ps1
```

The setting tests cover both server consoles, Hip ACK matching, timeout,
manual retry, firmware rejection, send failure, and phone/Hip reconnects.
The notification tests use a real loopback TLS gateway, the supplied server
and simulated ESP events. They check that already-created events can still
be delivered without a phone setting request or rep session, reconnect replay,
receipt acknowledgement, and rejection of the removed AR setting command.
OpenSSL is required for the TLS test.
Physical firmware flashing, APK installation and live IMU/phone acceptance
must be repeated on the deployed devices.
