# KAGÉ VR Stream — guide agent

Streaming PCVR PC Windows → Meta Quest (ici **Quest 3S**, `panther`). Réponses à l'utilisateur en **français**, concises. Priorité utilisateur : **optimisation/latence**, puis ergonomie.

## Règles
- Ne jamais inventer de mesure/résultat ; « Non disponible » si non mesuré. Mettre à jour `docs/STATUS.md` après chaque test réel.
- Pas de crypto maison. Pas de modification pare-feu/Defender/système sans accord explicite.
- Économiser les tokens : ne pas relire les fichiers déjà connus, éditer ciblé, sorties de build filtrées (`Select-String error|warning`).
- Commits : petits, message anglais, finir par `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Arborescence
- `core/` lib C++20 sans dépendance (protocole `kage/protocol/*`, UDP `kage/net`, horloge `kage/sync`, stats `kage/telemetry`, services `kage/session`). Compile Windows + Android.
- `apps/windows-host/` CLI `kage-host` (probe/serve/discover/ping).
- `apps/quest-client/` APK NativeActivity (OpenXR + GLES3), Gradle 9.9 / AGP 9.5.1, compileSdk 37, NDK 27.2.12479018.
- `drivers/steamvr/` pilote OpenVR (DLL chargée par vrserver.exe) = moteur de streaming PC.
- `apps/windows-gui/` interface WPF (.NET).
- `third_party/` en-têtes OpenVR (BSD-3), AMF (MIT), Monocypher 4.0.2 (BSD-2/CC0, sha256 paquet 38d07179…879c), stb_truetype.
- Sécurité : `core/include/kage/crypto/secure.hpp`. Clés PC `%LOCALAPPDATA%KageVRhost.key` + `paired.txt` (lus par kage-host ET le pilote) ; casque `client.key` + `hosts.txt` dans internalDataPath. Seul `SecureHello`/`SecureAccept` passe en clair sur 47802.
- `tests/` harness maison `tests/test_main.hpp` + CTest.

## Commandes (PowerShell)
```powershell
# PATH à jour si cmake introuvable :
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
cmake -S . -B build -G "Visual Studio 17 2022" -A x64; cmake --build build --config Release; ctest --test-dir build -C Release
# APK
$env:JAVA_HOME="C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot"; cd apps\quest-client; .\gradlew.bat assembleDebug -q
$adb="$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"; & $adb install -r app\build\outputs\apk\debug\app-debug.apk
& $adb shell am start -n dev.kage.vrstream/android.app.NativeActivity; & $adb logcat -d -s KageVR:*
```

## Environnement connu
Win11 Pro, Ryzen 5 5600X, RX 6700 XT (AMF 1.4.37, HEVC+H.264 matériel, **pas d'AV1**), VS2022 MSVC 14.44, CMake 4.4.4, JDK 17, SteamVR installé (pilote Virtual Desktop aussi enregistré ; runtime OpenXR actif = Virtual Desktop). PC 192.168.1.10 Wi-Fi, casque 192.168.1.100 (5 GHz canal 64). Quest : 1680×1760/œil, 72/80/90/120 Hz, Adreno 740.

## Test réel
`.\scripts\dev-e2e.ps1 [-NoInstall]` (connexion auto + proximité désactivée) ; `KAGE_DUMP_FRAME=1` pour une image composée en BMP. GUI : `dotnet build apps\windows-gui\KageVR.Gui.csproj -c Release -o build\gui`. Pilote déjà enregistré via vrpathreg (`kage-host remove-driver` pour retirer). Pièges déjà rencontrés : ne jamais appeler AMediaCodec depuis ses callbacks ; vider l'ImageReader en permanence ; lier le cbuffer au VS ; verrou ID3D10Multithread autour de la composition (contexte partagé avec AMF).

## Ports UDP
47800 découverte · 47801 contrôle (kage-host) · 47802 streaming (pilote SteamVR).

## Architecture du streaming
Quest → `StreamHello` (FOV, IPD, Hz, résolution) → pilote ; Quest → `TrackingState` à chaque frame ; pilote → `VideoFrame` fragmenté (HEVC AMF, côte à côte, pose de rendu incluse) ; Quest soumet la couche de projection avec **la pose de rendu** pour que le runtime fasse la reprojection. Images incomplètes abandonnées + demande IDR. Audio : canal 5, u32 fréquence + PCM int16 stéréo. Avant tout : PairRequest (47801, code 6 chiffres confirmé dans la GUI) puis SecureHello/SecureAccept (47802), ensuite tout est chiffré (+24 o/paquet).
