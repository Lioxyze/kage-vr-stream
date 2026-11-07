# Builds everything and assembles a ready-to-use folder: dist\KageVR
#   KageVR.exe (+ .NET files)  kage-host.exe  driver\kage\  KageVR-Quest.apk  LISEZMOI.txt
# Usage: .\scripts\package.ps1 [-SkipBuild]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = "$root\dist\KageVR"
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not $SkipBuild) {
    cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 | Out-Null
    cmake --build "$root\build" --config Release | Select-String ' error '
    dotnet build "$root\apps\windows-gui\KageVR.Gui.csproj" -c Release -o "$root\build\gui" | Select-String ' error '
    $env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot'
    Push-Location "$root\apps\quest-client"; .\gradlew.bat assembleDebug -q; Pop-Location
}
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force "$out\driver" | Out-Null
Copy-Item "$root\build\gui\*" $out -Recurse
Copy-Item "$root\build\apps\windows-host\Release\kage-host.exe" $out
Copy-Item "$root\build\driver\kage" "$out\driver\kage" -Recurse
Copy-Item "$root\apps\quest-client\app\build\outputs\apk\debug\app-debug.apk" "$out\KageVR-Quest.apk"
Copy-Item "$root\packaging\AGENTS.md" $out  # guide for an AI assistant troubleshooting on the user's PC
# USB cable mode: adb (Android platform-tools, Apache 2.0) for `adb reverse`.
$pt = "$env:LOCALAPPDATA\Android\Sdk\platform-tools"
if (Test-Path "$pt\adb.exe") { New-Item -ItemType Directory -Force "$out\adb" | Out-Null; Copy-Item "$pt\adb.exe","$pt\AdbWinApi.dll","$pt\AdbWinUsbApi.dll" "$out\adb" }
@'
KAGE VR Stream - installation
=============================
PC (Windows, SteamVR installe) :
  1. Lancer KageVR.exe (Windows peut demander d'autoriser le reseau prive : accepter).
  2. Cliquer "Installer le pilote SteamVR" (une seule fois ; refaire si ce dossier est deplace).
Casque Quest (mode developpeur active, cable USB) :
  3. Installer KageVR-Quest.apk avec SideQuest ou :  adb install -r KageVR-Quest.apk
  4. Dans le casque : Bibliotheque > Sources inconnues > KAGE VR Stream, choisir le PC.
  5. Premiere fois : comparer le code a 6 chiffres casque / PC et accepter sur le PC.
Full body Rift CV1 : onglet "Full body" de KageVR.exe (etapes 1-2-3 + cases a cocher).
Reglage conseille dans SteamVR : Video > Resolution de rendu = 100 %.
'@ | Set-Content "$out\LISEZMOI.txt" -Encoding utf8
Write-Host "Pret : $out"
