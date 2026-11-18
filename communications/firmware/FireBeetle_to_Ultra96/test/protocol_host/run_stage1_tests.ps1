$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force '.build' | Out-Null
    $firmware = 'communications/firmware/FireBeetle_to_Ultra96'
    $core = "$firmware/components/hardware_core"
    $tests = "$firmware/test/protocol_host"
    $cppIncludes = @('-Ihardware/tests/stubs', '-Ihardware/node', "-I$core/include")
    function Build-And-Run($compiler, $name, $arguments) {
        & $compiler @arguments -o ".build/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
        & ".build/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
    }
    $cppFlags = @('-std=c++17', '-Wall', '-Wextra', '-Werror') + $cppIncludes
    $feedbackSources = @('hardware/node/BuzzerFeedback.cpp', "$core/src/FeedbackController.cpp", "$core/src/FeedbackDiagnostic.cpp")
    Build-And-Run 'g++' 'test_feedback' ($cppFlags + $feedbackSources + "$tests/test_feedback.cpp")
    Build-And-Run 'g++' 'test_feedback_bridge' ($cppFlags + @(
        "-I$tests/stubs", '-DAPP_USE_MOCK_IMU=1', '-DAPP_FEEDBACK_DIAGNOSTICS=1'
    ) + $feedbackSources + @(
        "$core/src/feedback_bridge.cpp", "$core/src/exercise_bridge.cpp",
        'hardware/node/RepCounter.cpp', 'hardware/node/ContinuousCounter.cpp',
        'hardware/node/NodeSession.cpp', "$tests/test_feedback_bridge.cpp"
    ))
    foreach ($test in @('protocol', 'sensor_history')) {
        Build-And-Run 'gcc' "test_$test" @(
            '-std=c11', '-Wall', '-Wextra', '-Werror', "-I$firmware/include",
            "$firmware/src/$test.c", "$tests/test_$test.c"
        )
    }
    Build-And-Run 'g++' 'test_generic_peak_detector' ($cppFlags + @(
        'hardware/node/GenericPeakDetector.cpp', "$tests/test_generic_peak_detector.cpp"
    ))
    Build-And-Run 'g++' 'test_hardware_sensor_history' ($cppFlags + @(
        'hardware/node/SensorHistory.cpp', 'hardware/tests/sensor_history_test.cpp'
    ))
    Build-And-Run 'g++' 'test_exercise_mock' ($cppFlags + @(
        "$tests/test_exercise_mock.cpp", 'hardware/node/RepCounter.cpp',
        'hardware/node/ContinuousCounter.cpp'
    ))
    Write-Output 'All seven Stage 1 host test executables passed.'
} finally {
    Pop-Location
}
