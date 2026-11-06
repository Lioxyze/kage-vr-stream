# Development end-to-end test: (re)starts kage-host serve, installs the APK,
# keeps the headset awake (proximity override), auto-connects, then prints logs.
# Usage: .\scripts\dev-e2e.ps1 [-Seconds 40] [-NoInstall]
param([int]$Seconds = 40, [switch]$NoInstall)
$root = Split-Path -Parent $PSScriptRoot
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$host_ = "$root\build\apps\windows-host\Release\kage-host.exe"
$driverLog = "$env:LOCALAPPDATA\KageVR\driver.log"

Get-Process vrserver, vrcompositor, vrmonitor, vrdashboard, vrwebhelper, vrstartup, kage-host -ErrorAction SilentlyContinue |
    Stop-Process -Force -Confirm:$false
Start-Sleep 1
if (Test-Path $driverLog) { Remove-Item $driverLog -Confirm:$false }
$serve = Start-Process -FilePath $host_ -ArgumentList 'serve', '--name', 'PC-KAGE' -PassThru -WindowStyle Hidden `
    -RedirectStandardError "$env:TEMP\kage-serve.log"

if (-not $NoInstall) { & $adb install -r "$root\apps\quest-client\app\build\outputs\apk\debug\app-debug.apk" | Select-Object -Last 1 }
& $adb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
& $adb shell setprop debug.kage.autoconnect 1
& $adb shell am force-stop dev.kage.vrstream
& $adb logcat -c
& $adb shell am start -n dev.kage.vrstream/android.app.NativeActivity | Out-Null
Start-Sleep $Seconds

Write-Host '--- quest ---'
& $adb logcat -d -s KageVR:* | Select-Object -Last 30
Write-Host '--- driver ---'
if (Test-Path $driverLog) { Get-Content $driverLog | Select-Object -Last 30 }
Write-Host '--- host ---'
Get-Content "$env:TEMP\kage-serve.log" -ErrorAction SilentlyContinue | Select-String -NotMatch 'discovery probe' | Select-Object -Last 8
