param([string]$PythonCommand = 'python', [switch]$SkipManaged)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/run_stage3_tests.ps1" -PythonCommand $PythonCommand -SkipManaged:$SkipManaged
if ($LASTEXITCODE -ne 0) { throw 'Stage 3 regressions failed' }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../../..')).Path
Push-Location "$repoRoot/communications/ultra96"
try {
    & $PythonCommand -m unittest -v test_stage4_idle
    if ($LASTEXITCODE -ne 0) { throw 'Stage 4 idle integration checks failed' }
} finally { Pop-Location }
