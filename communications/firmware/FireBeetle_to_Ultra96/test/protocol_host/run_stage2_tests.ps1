param([string]$PythonCommand = 'python')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
Push-Location $repoRoot
try {
    & "$PSScriptRoot/run_stage1_tests.ps1"
    $firmware = 'communications/firmware/FireBeetle_to_Ultra96'
    foreach ($name in @('test_feedback_delivery', 'feedback_peer')) {
        gcc -std=c11 -Wall -Wextra -Werror "-I$firmware/include" `
            "$firmware/src/protocol.c" "$firmware/src/feedback_delivery.c" `
            "$firmware/test/protocol_host/$name.c" -o ".build/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    }
    & .build/test_feedback_delivery.exe
    if ($LASTEXITCODE -ne 0) { throw 'C feedback delivery checks failed' }
    Push-Location communications/ultra96
    try {
        & $PythonCommand -m unittest test_feedback_delivery test_server_ai_feedback test_server test_window_assembler `
            test_visualizer_protocol test_visualizer_gateway
        if ($LASTEXITCODE -ne 0) { throw 'Python Stage 2/regression checks failed' }
    } finally { Pop-Location }
} finally { Pop-Location }
