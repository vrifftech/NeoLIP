param(
  [string]$Root = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Root)) {
  if ((Test-Path (Join-Path $PSScriptRoot "bin")) -and (Test-Path (Join-Path $PSScriptRoot "models"))) {
    $Root = $PSScriptRoot
  } else {
    $Root = Split-Path -Parent $PSScriptRoot
  }
}

$Root = (Resolve-Path $Root).Path
$Backend = Join-Path $Root "bin/neolip_backend.exe"
if (-not (Test-Path $Backend)) { $Backend = Join-Path $Root "bin/neolip_backend" }
if (-not (Test-Path $Backend)) { throw "NeoLIP private backend not found: $Backend" }

$TempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("neolip_smoke_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
try {
  & $Backend check-bundle $Root
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  & $Backend text-to-ntp (Join-Path $Root "examples/test.txt") (Join-Path $TempRoot "smoke.ntp") --models (Join-Path $Root "models")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  & $Backend align-wav-text-lip (Join-Path $Root "examples/test.wav") (Join-Path $Root "examples/test.txt") (Join-Path $TempRoot "smoke.lip") --models (Join-Path $Root "models") --work-dir (Join-Path $TempRoot "smoke_work")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

  & $Backend lip-info (Join-Path $TempRoot "smoke.lip")
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
  Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}
