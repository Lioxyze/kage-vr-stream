# Compiler KAGÉ VR Stream

Terminal : **PowerShell** (Windows PowerShell 5.1 ou PowerShell 7).

## Hôte Windows + tests

Prérequis vérifiés sur la machine de développement :
- Visual Studio 2022 Community 17.14, charge de travail C++ (MSVC 14.44, SDK Windows 10.0.22621)
- CMake ≥ 3.22 (`winget install Kitware.CMake`, testé avec 4.4.4)

```powershell
.\scripts\build-host.ps1
```

Ou à la main :

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Exécutable : `build\apps\windows-host\Release\kage-host.exe`.

## Client Quest (APK)

Prérequis :
1. JDK 17 (`winget install Microsoft.OpenJDK.17`, déjà installé ici).
2. Android Studio (`winget install Google.AndroidStudio`). L'installation demande d'accepter la licence du SDK Android : c'est à l'utilisateur de le faire.
3. Dans Android Studio > SDK Manager :
   - SDK Platforms : **API 37** (c'est la valeur de `compileSdk` ; testé avec android-37.0)
   - SDK Tools : **NDK (Side by side) 27.2.12479018**, **CMake 3.22.1**, **Android SDK Platform-Tools** (adb)
4. Le Gradle wrapper (Gradle 9.9.0, AGP 9.5.1) est versionné dans `apps/quest-client` : il n'y a rien à installer de plus.

```powershell
.\scripts\build-quest.ps1            # produit app-debug.apk
.\scripts\build-quest.ps1 -Install   # + adb install sur le casque
```

### Préparer le Quest 3

1. Créer ou utiliser un compte développeur Meta, puis activer le **mode développeur** dans l'application mobile Meta Horizon.
2. Brancher le casque en USB-C et accepter « Autoriser le débogage USB » dans le casque.
3. Vérifier la connexion : `adb devices`.
4. Après l'installation, l'application se trouve dans la Bibliothèque > *Sources inconnues*.
5. Journaux : `adb logcat -s KageVR`.

## Test manuel de la phase 1 (PC + casque)

1. PC : `kage-host.exe serve`. Accepter la demande du pare-feu Windows **uniquement pour les réseaux privés**.
2. Casque : lancer KAGÉ VR Stream. Le fond est violet foncé pendant la recherche, cyan une fois connecté, orange si l'hôte ne répond plus.
3. `adb logcat -s KageVR` doit afficher :
   - `OpenXR runtime: ...` et `eye 0: recommended WxH`, avec les valeurs réelles du casque
   - `refresh rates supported: ...`
   - `host found: '<nom du PC>'`
   - toutes les 5 s : `link: rtt median ...`. C'est la première mesure Wi-Fi réelle PC↔Quest.
4. Reporter ces valeurs dans `docs/STATUS.md`.
