param([string]$PythonCommand = 'python', [switch]$SkipManaged)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
Push-Location $repoRoot
try {
    & "$PSScriptRoot/run_stage2_tests.ps1" -PythonCommand $PythonCommand
    if (-not $SkipManaged) { & "$repoRoot/communications/unity/tests/run_protocol_host.ps1" }
    Push-Location communications/ultra96
    try {
        & $PythonCommand -m unittest test_rep_control
        if ($LASTEXITCODE -ne 0) { throw 'Stage 3 rep integration tests failed' }
    } finally { Pop-Location }
} finally { Pop-Location }
