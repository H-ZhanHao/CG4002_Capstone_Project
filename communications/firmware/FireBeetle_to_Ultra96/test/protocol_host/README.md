# Portable firmware and protocol checks

Reviewed against the repository on 16 November 2026. Run the commands below
from the **repository root**, with desktop GCC/G++ and Python available.
They compile production logic or run simulated server tests; they do not flash
boards, validate actual GPIO timing or run DPU models.

Some files in this test tree are ignored by Git. Confirm the referenced test
sources are included in your checkout/submission before using them. The current
runtime builds canonical C++ modules from `hardware/node`; do not use removed
component-local copies of GenericPeakDetector or SensorHistory.

## Protocol and ordered history

Create an output directory, then compile/run the production C codec tests:

```powershell
New-Item -ItemType Directory -Force .build | Out-Null
gcc -std=c11 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/include `
  communications/firmware/FireBeetle_to_Ultra96/src/protocol.c `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_protocol.c `
  -o .build/protocol_host_tests.exe
& .build/protocol_host_tests.exe
```

Independent golden vectors check encoding/decoding rather than constructing
expected bytes through the codec under test. To check the sequence-aware feeder:

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/include `
  communications/firmware/FireBeetle_to_Ultra96/src/sensor_history.c `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_sensor_history.c `
  -o .build/sensor_history_tests.exe
& .build/sensor_history_tests.exe
```

To check live-only joining, immediate startup, queue retry and stop ordering:

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/include `
  communications/firmware/FireBeetle_to_Ultra96/src/sensor_history.c `
  communications/firmware/FireBeetle_to_Ultra96/one_node_tests/test_live_history.c `
  -o .build/one_node_live_test.exe
& .build/one_node_live_test.exe
```

## Canonical hardware and final fall control

The fall runner compiles the canonical detector, communications bridge and
current fall-setting codec:

```powershell
& ./communications/firmware/FireBeetle_to_Ultra96/fall_tests/run_tests.ps1
```

For the generic peak detector and separate canonical history regression:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror -Ihardware/node `
  hardware/node/GenericPeakDetector.cpp `
  communications/firmware/FireBeetle_to_Ultra96/test/protocol_host/test_generic_peak_detector.cpp `
  -o .build/generic_peak_detector_tests.exe
& .build/generic_peak_detector_tests.exe

g++ -std=c++17 -Wall -Wextra -Werror -Ihardware/node `
  hardware/node/SensorHistory.cpp hardware/tests/sensor_history_test.cpp `
  -o .build/hardware_sensor_history_tests.exe
& .build/hardware_sensor_history_tests.exe
```

These tests do not imply the canonical C++ SensorHistory class runs in the
communications firmware; its transport history is `src/sensor_history.c`.
Additional connection-LED coverage is in
[the final-profile host fixture](../../one_node_tests/test_connection_led.cpp).

## Current integrated server checks

From the repository root:

```powershell
python -B -m unittest discover -s server -p 'test_one_node_integration.py' -v
python -B -m unittest discover -s server -p 'test_server_fall_control.py' -v
```

These exercise dynamic capture, real-only windows and acknowledged settings with
simulated connections. Gateway/phone notification tests additionally require
`websockets==12.0` and OpenSSL; see
[Ultra96 verification](../../../../ultra96/README.md#verification-and-known-test-limitations).
The Unity managed helper needs .NET plus generated Unity projects/references;
see [Unity tests](../../../../unity/Ultra96VisualizerDemo/README.md#tests).

## Historical stage runners and evidence

`run_stage1_tests.ps1` through `run_stage5_tests.ps1` and the old integrated
feedback wrapper were authored around staged revisions. Several Python tests
have since moved under `communications/ultra96/test` and still import the
removed `server_AI.py`/`server_new.py`; some wrappers also calculate paths for
the old layout. Do not present them as a verified one-command final regression
suite or create aliases to conceal those differences. Their test maintenance is
separate from this guide refresh.

The annotated [Stage 1](STAGE1_CHECKPOINT.md), [Stage 2](STAGE2_CHECKPOINT.md),
[Stage 3](STAGE3_CHECKPOINT.md), [Stage 4](STAGE4_CHECKPOINT.md) and
[Stage 5](STAGE5_CHECKPOINT.md) records preserve tested source/image archives,
old outputs and confirmation limits. Use matching saved source/images to
reproduce those historical stages; old profile names are not current builds.
Historical pass counts are collected in
[Experiments and results](../../../../docs/EXPERIMENTS_AND_RESULTS.md#6-feedback-idle-and-fall-checkpoints).

Current physical acceptance still needs final firmware/device checks: actual
100 Hz cadence, history catch-up, body axes, independent joining, output timing,
Hip enable/disable and detector-to-phone notification. Synthetic fall injection
is a marked delivery diagnostic, not a measurement of detector sensitivity.
