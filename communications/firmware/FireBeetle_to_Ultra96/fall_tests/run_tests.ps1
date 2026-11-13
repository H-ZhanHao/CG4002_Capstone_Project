$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
Push-Location $repoRoot
try {
    $core = 'communications/firmware/FireBeetle_to_Ultra96/components/hardware_core'
    $build = '.build/fall_bridge_validation'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    g++ -std=c++11 -Wall -Wextra -Werror -Ihardware/node "-I$core/include" `
        hardware/node/FallDetector.cpp "$core/src/fall_bridge.cpp" `
        hardware/node/BuzzerFeedback.cpp "$core/src/FeedbackController.cpp" `
        "$PSScriptRoot/test_fall_bridge.cpp" -o "$build/test_fall_bridge.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Fall bridge compilation failed' }
    & "$build/test_fall_bridge.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Fall bridge tests failed' }

    g++ -std=c++11 -Wall -Wextra -Werror -Ihardware/node `
        hardware/node/FallDetector.cpp hardware/tests/fall_detector_test.cpp `
        -o "$build/fall_detector_test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Hardware fall detector compilation failed' }
    & "$build/fall_detector_test.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Hardware fall detector tests failed' }
} finally {
    Pop-Location
}
