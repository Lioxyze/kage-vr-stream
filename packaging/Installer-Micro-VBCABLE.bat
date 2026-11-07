@echo off
rem Telecharge VB-CABLE (VB-Audio, gratuit, signe) depuis le site officiel et lance son installation.
rem VB-CABLE cree le micro Windows "CABLE Output" qui recoit la voix du casque.
echo Telechargement de VB-CABLE depuis vb-audio.com ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d=Join-Path $env:USERPROFILE 'Downloads\VBCABLE'; New-Item -ItemType Directory -Force $d | Out-Null; Invoke-WebRequest 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip' -OutFile (Join-Path $d 'vbcable.zip') -UseBasicParsing; Expand-Archive (Join-Path $d 'vbcable.zip') (Join-Path $d 'pack') -Force; $s=Get-AuthenticodeSignature (Join-Path $d 'pack\VBCABLE_Setup_x64.exe'); if ($s.Status -ne 'Valid') { Write-Host 'Signature invalide : installation annulee.'; exit 1 }; Start-Process (Join-Path $d 'pack\VBCABLE_Setup_x64.exe') -Verb RunAs -WorkingDirectory (Join-Path $d 'pack')"
echo.
echo Dans la fenetre VB-CABLE : cliquer "Install Driver", puis REDEMARRER le PC.
pause