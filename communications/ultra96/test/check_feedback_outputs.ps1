param([string]$PythonCommand = 'python')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force '.build' | Out-Null
    $fw = 'communications/firmware/FireBeetle_to_Ultra96'
    $core = "$fw/components/hardware_core"
    g++ -std=c++17 -Wall -Wextra -Werror -Ihardware/node "-I$core/include" `
        hardware/node/BuzzerFeedback.cpp "$core/src/FeedbackController.cpp" `
        "$core/src/FeedbackDiagnostic.cpp" hardware/tests/integrated_feedback_test.cpp `
        -o .build/integrated_feedback_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Integrated output test compilation failed' }
    & .build/integrated_feedback_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Integrated output tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror "-I$fw/include" "$fw/src/protocol.c" `
        "$fw/src/feedback_delivery.c" hardware/tests/integrated_feedback_peer.c `
        -o .build/integrated_feedback_peer.exe
    if ($LASTEXITCODE -ne 0) { throw 'Feedback wire peer compilation failed' }
    Push-Location $PSScriptRoot
    try {
        & $PythonCommand -m unittest -v check_feedback_outputs
        if ($LASTEXITCODE -ne 0) { throw 'Integrated feedback checks failed' }
    } finally { Pop-Location }
} finally { Pop-Location }
