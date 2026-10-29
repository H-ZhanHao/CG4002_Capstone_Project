param(
  [Parameter(Mandatory = $true)]
  [ValidatePattern("^COM\d+$")]
  [string]$Port
)

if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
  Write-Error "PlatformIO CLI 'pio' was not found. Run this script from a PlatformIO terminal."
  exit 1
}

& pio device monitor --port $Port --baud 115200
exit $LASTEXITCODE
