# LIO VR

Streaming PCVR d'un PC Windows (SteamVR) vers un Meta Quest, en Wi-Fi ou par câble USB. « LIO VR » est le nom affiché. Dans le code, les noms techniques restent ceux du projet d'origine KAGÉ VR Stream : `KageVR.exe`, `kage-host`, le pilote SteamVR `kage`.

> **État :** sessions PCVR réelles sur Quest 3S à 72, 90 et 120 i/s, en Wi-Fi ou en USB. Fonctionnent : HEVC AMF avec tranches, chiffrement, audio, micro, full body Rift CV1, débit adaptatif avec mesure du Wi-Fi au démarrage, et mises à jour intégrées. Mesures détaillées : [docs/STATUS.md](docs/STATUS.md).

## Utilisation

1. Télécharger la dernière version : `KageVR-Portable.zip` dans les [releases](https://github.com/Lioxyze/kage-vr-releases/releases). L'extraire, puis lancer `KageVR.exe`.
2. Une seule fois : cliquer sur **Installer le pilote SteamVR**.
3. Dans le casque : *Bibliothèque > Sources inconnues > LIO VR*. Choisir ton PC : « (Wi-Fi) » ou « (USB) » si le câble est branché.
4. Première connexion : comparer le code à 6 chiffres affiché dans le casque et sur le PC, puis accepter sur le PC.
5. En jeu : un **appui long sur ☰** (manette gauche), ou X+Y maintenus, ouvre le menu (Recentrer, netteté, micro, full body, déconnexion).
6. Mises à jour : quand KageVR affiche « Mettre à jour ● », un clic suffit. Le PC est mis à jour, et le casque aussi s'il est branché.

## Développement

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64; cmake --build build --config Release; ctest --test-dir build -C Release
.\build\apps\windows-host\Release\kage-host.exe probe                 # diagnostic (GPU, encodeurs, SteamVR, Wi-Fi)
.\build\drivers\steamvr\Release\kage-encbench.exe [charge]            # latence d'encodage AMF, sans casque
.\build\drivers\steamvr\Release\kage-bitrate-replay.exe               # rejoue driver.log dans le contrôleur de débit
```

`ctest` lance aussi `test_stream_e2e` : un faux casque se connecte au vrai serveur de streaming du pilote par UDP, sur ce PC.

```
core/                    bibliothèque C++20 partagée (protocole, réseau, crypto, synchro)
apps/windows-host/       kage-host : découverte, appairage, pont USB, full body Rift, diagnostic
apps/windows-gui/        KageVR.exe (WPF) : interface PC « LIO VR »
apps/quest-client/       APK Quest native (OpenXR + GLES, Gradle/NDK)
drivers/steamvr/         pilote OpenVR : composition, encodage AMF/MF, streaming
tests/                   tests unitaires et d'intégration (CTest)
scripts/                 compilation, paquet portable, publication
docs/                    architecture, compilation, état et mesures
```

- [Décisions d'architecture](docs/ARCHITECTURE.md)
- [Compilation et installation](docs/BUILD.md)
- [État des fonctionnalités et mesures](docs/STATUS.md)

## Licence

À définir par le propriétaire du projet. MIT ou Apache-2.0 sont suggérées : elles sont compatibles avec OpenVR (BSD-3), AMF (MIT), Monocypher (BSD-2/CC0) et le loader OpenXR (Apache-2.0).
