param([string]$PythonCommand = 'python', [switch]$SkipManaged, [switch]$OnlyStage5)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
Push-Location $repoRoot
try {
    if (-not $OnlyStage5) {
        & "$PSScriptRoot/run_stage4_tests.ps1" -PythonCommand $PythonCommand -SkipManaged:$SkipManaged
        if ($LASTEXITCODE -ne 0) { throw 'Stage 4 regressions failed' }
    }
    $fw = 'communications/firmware/FireBeetle_to_Ultra96'
    $core = "$fw/components/hardware_core"
    g++ -std=c++17 -Wall -Wextra -Werror -Ihardware/tests/stubs -Ihardware/node "-I$core/include" `
        hardware/node/FallDetector.cpp hardware/node/BuzzerFeedback.cpp "$core/src/fall_bridge.cpp" "$core/src/FeedbackController.cpp" `
        "$PSScriptRoot/test_fall_bridge.cpp" -o .build/test_fall_bridge.exe
    if ($LASTEXITCODE -ne 0) { throw 'Fall adapter compilation failed' }
    & .build/test_fall_bridge.exe
    if ($LASTEXITCODE -ne 0) { throw 'Fall adapter tests failed' }
    g++ -std=c++17 -Wall -Wextra -Werror -Ihardware/tests/stubs -Ihardware/node "-I$core/include" `
        "-I$PSScriptRoot/stubs" -DAPP_FEEDBACK_DIAGNOSTICS=1 "$core/src/feedback_bridge.cpp" `
        "$core/src/FeedbackController.cpp" hardware/node/BuzzerFeedback.cpp "$core/src/FeedbackDiagnostic.cpp" `
        "$PSScriptRoot/test_fall_diagnostics.cpp" -o .build/test_fall_diagnostics.exe
    if ($LASTEXITCODE -ne 0) { throw 'Fall diagnostic compilation failed' }
    & .build/test_fall_diagnostics.exe
    if ($LASTEXITCODE -ne 0) { throw 'Fall diagnostic checks failed' }
    foreach ($name in @('test_device_events', 'device_event_peer')) {
        gcc -std=c11 -Wall -Wextra -Werror "-I$fw/include" "$fw/src/protocol.c" "$fw/src/device_events.c" `
            "$PSScriptRoot/$name.c" -o ".build/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Device events compilation failed: $name" }
    }
    & .build/test_device_events.exe
    if ($LASTEXITCODE -ne 0) { throw 'Device event tests failed' }
    Push-Location communications/ultra96
    try {
        & $PythonCommand -m unittest -v test_stage5_fall
        if ($LASTEXITCODE -ne 0) { throw 'Stage 5 server integration tests failed' }
    } finally { Pop-Location }
} finally { Pop-Location }
