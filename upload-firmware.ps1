param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("hip", "wrist")]
  [string]$Role,

  [Parameter(Mandatory = $true)]
  [ValidatePattern("^COM\d+$")]
  [string]$Port
)

$environment = if ($Role -eq "hip") {
  "node1_private_tls"
} else {
  "node2_private_tls"
}

if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
  Write-Error "PlatformIO CLI 'pio' was not found. Run this script from a PlatformIO terminal."
  exit 1
}

Push-Location $PSScriptRoot
try {
  & pio run -e $environment -t upload --upload-port $Port
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
} finally {
  Pop-Location
}
