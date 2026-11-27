# Build the co-op player folder from source (see BUILDING.md). Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File Co-op\build_coop.ps1
# -Setup first installs the build tools (MSYS2 + Python 3.12, Savage Wind's own tools\setup_windows.ps1).
param([switch]$Setup)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Setup) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'tools\setup_windows.ps1')
    if ($LASTEXITCODE) { throw 'tools\setup_windows.ps1 failed' }
}
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    throw "MSYS2 is not installed. Run again with -Setup (installs MSYS2 and Python 3.12), or see BUILDING.md."
}
$env:CHERE_INVOKING = '1'
$env:MSYSTEM = 'UCRT64'
Push-Location $root
try {
    & $bash -lc 'bash Co-op/build_coop.sh'
    if ($LASTEXITCODE) { throw "the build failed (exit $LASTEXITCODE)" }
} finally { Pop-Location }
