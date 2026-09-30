param(
    [string]$SketchbookPath = (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Arduino')
)

$ErrorActionPreference = 'Stop'
$librarySource = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'node')).Path
if (-not (Test-Path -LiteralPath (Join-Path $librarySource 'library.properties'))) {
    throw 'The shared ExerciseNode library is missing.'
}
$libraryParent = Join-Path ([IO.Path]::GetFullPath($SketchbookPath)) 'libraries'
$libraryLink = Join-Path $libraryParent 'ExerciseNode'

if (Test-Path -LiteralPath $libraryLink) {
    $existing = Get-Item -LiteralPath $libraryLink -Force
    if ($existing.LinkType -eq 'Junction' -and @($existing.Target) -contains $librarySource) {
        Write-Output "ExerciseNode is already linked to $librarySource"
        return
    }
    throw "An existing library is at $libraryLink; it has been left unchanged."
}

# A junction keeps one live copy of the counters and ExerciseConfig.h. It does
# not copy files, change board packages, or require editing the two sketches.
New-Item -ItemType Directory -Path $libraryParent -Force | Out-Null
New-Item -ItemType Junction -Path $libraryLink -Target $librarySource | Out-Null
Write-Output "Arduino library registered: $libraryLink -> $librarySource"
Write-Output 'Reopen Arduino IDE if it was already running.'
