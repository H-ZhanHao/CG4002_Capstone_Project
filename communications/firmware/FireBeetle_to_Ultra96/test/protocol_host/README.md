# Protocol host tests

These tests compile the production `protocol.c` directly with a desktop C
compiler. The expected byte arrays are written independently from the codec so
they detect byte-order and layout regressions.

From the repository root on Windows with GCC available:

```powershell
gcc -std=c11 -Wall -Wextra -Werror `
  -Ifirmware/FireBeetle_to_Ultra96/include `
  firmware/FireBeetle_to_Ultra96/src/protocol.c `
  firmware/FireBeetle_to_Ultra96/test/protocol_host/test_protocol.c `
  -o .tmp/protocol_host_tests.exe
.tmp/protocol_host_tests.exe
```
