# Protocol host tests

Stage 3 adds phone session/rep contracts, loopback IPC, real WSS integration,
and the checked-in C# transport. Run `run_stage3_tests.ps1`; see
[the Stage 3 checkpoint](STAGE3_CHECKPOINT.md) for dependencies, source/flash
archives, Unity APIs, and simulator/physical test instructions.

Stage 2 adds feedback codecs, replay protection, and server-to-two-C-peer tests:

```powershell
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage2_tests.ps1
```

See [the Stage 2 checkpoint](STAGE2_CHECKPOINT.md) for the wire layout, saved
firmware versions, automated results, and physical TLS test commands.

For Stage 1 local LED/buzzer feedback and the existing firmware regressions, run
all seven host executables from the repository root:

```powershell
& communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/run_stage1_tests.ps1
```

See [the Stage 1 checkpoint](STAGE1_CHECKPOINT.md) for the changed files, build
commands, USB diagnostic commands, physical test procedure, and limitations.

These tests compile the production `protocol.c` directly with a desktop C
compiler. The expected byte arrays are written independently from the codec so
they detect byte-order and layout regressions.

From the repository root on Windows with GCC available:

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/include `
  communications/firmware/FireBeetle_to_Ultra96/src/protocol.c `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_protocol.c `
  -o .tmp/protocol_host_tests.exe
.tmp/protocol_host_tests.exe
```

The history/feeder test is independent of ESP-IDF and verifies a three-second
historical flush followed by live delivery, chronological exactly-once capture
sequences, and bounded-ring overwrite reporting:

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/include `
  communications/firmware/FireBeetle_to_Ultra96/src/sensor_history.c `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_sensor_history.c `
  -o .tmp/sensor_history_tests.exe
.tmp/sensor_history_tests.exe
```

The copied C++ generic detector has a focused communications-side regression
test for its 100 Hz duration guard and weak sustained peaks:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/components/hardware_core/include `
  communications/firmware/FireBeetle_to_Ultra96/components/hardware_core/src/GenericPeakDetector.cpp `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_generic_peak_detector.cpp `
  -o .tmp/generic_peak_detector_tests.exe
.tmp/generic_peak_detector_tests.exe
```

The copied C++ sensor-history implementation is likewise checked with the
hardware team's original host test. This protects the recognizable hardware
module even though the communications-specific sequence-aware feeder lives in
`src/sensor_history.c`:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/components/hardware_core/include `
  communications/firmware/FireBeetle_to_Ultra96/components/hardware_core/src/SensorHistory.cpp `
  hardware/tests/sensor_history_test.cpp `
  -o .tmp/hardware_sensor_history_tests.exe
.tmp/hardware_sensor_history_tests.exe
```

The exercise mock integration test proves that the deterministic firmware
waveform drives the hardware team's original pause and continuous counters
exactly once:

```powershell
g++ -std=c++11 -Wall -Wextra -Werror `
  -Ihardware/tests/stubs -Ihardware/node `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_exercise_mock.cpp `
  hardware/node/RepCounter.cpp hardware/node/ContinuousCounter.cpp `
  -o .build/test_exercise_mock.exe
& .build/test_exercise_mock.exe
```
