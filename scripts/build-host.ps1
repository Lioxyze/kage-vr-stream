# Builds the core library, the Windows host and the tests, then runs the tests.
# Usage (PowerShell):  .\scripts\build-host.ps1 [-Config Release|Debug] [-SkipTests]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'cmake introuvable. Installer : winget install Kitware.CMake (puis rouvrir le terminal).'
}

cmake -S $root -B "$root\build" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'configuration CMake échouée' }
cmake --build "$root\build" --config $Config
if ($LASTEXITCODE) { throw 'compilation échouée' }

if (-not $SkipTests) {
    ctest --test-dir "$root\build" -C $Config --output-on-failure
    if ($LASTEXITCODE) { throw 'des tests ont échoué' }
}
Write-Host "OK: $root\build\apps\windows-host\$Config\kage-host.exe"
