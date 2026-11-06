# Builds the Quest client APK (arm64-v8a, debug-signed) and optionally installs it.
# Prerequisites: JDK 17+, Android SDK (platform 34, build-tools), NDK 27.2.12479018,
# CMake 3.22.1 from the SDK Manager. See docs/BUILD.md.
# Usage (PowerShell):  .\scripts\build-quest.ps1 [-Install]
param([switch]$Install)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $root 'apps\quest-client'

if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem 'C:\Program Files\Microsoft' -Filter 'jdk-17*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $jdk) { throw 'JDK 17 introuvable : définir JAVA_HOME.' }
    $env:JAVA_HOME = $jdk.FullName
}
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path $sdk)) { throw "Android SDK introuvable ($sdk). Installer Android Studio puis le SDK/NDK." }
Set-Content -Path (Join-Path $proj 'local.properties') -Value ("sdk.dir=" + ($sdk -replace '\\', '/')) -Encoding ascii

if (-not (Test-Path (Join-Path $proj 'gradlew.bat'))) {
    throw 'Gradle wrapper absent : voir docs/BUILD.md (section "Gradle wrapper").'
}
Push-Location $proj
try {
    .\gradlew.bat assembleDebug
    if ($LASTEXITCODE) { throw 'compilation APK échouée' }
    $apk = Join-Path $proj 'app\build\outputs\apk\debug\app-debug.apk'
    Write-Host "OK: $apk"
    if ($Install) {
        & (Join-Path $sdk 'platform-tools\adb.exe') install -r $apk
        if ($LASTEXITCODE) { throw 'adb install échoué (casque branché ? mode développeur ? autorisation USB acceptée ?)' }
    }
} finally { Pop-Location }
