# Protocol host tests

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

The history/feeder test is independent of ESP-IDF and verifies a five-second
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

The copied C++ generic detector is checked with the hardware team's original
host test so filtering, duration qualification, cooldown, gaps, invalid values
and timer rollover remain equivalent:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror `
  -Icommunications/firmware/FireBeetle_to_Ultra96/components/hardware_core/include `
  communications/firmware/FireBeetle_to_Ultra96/components/hardware_core/src/GenericPeakDetector.cpp `
  hardware/tests/generic_peak_detector_test.cpp `
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
