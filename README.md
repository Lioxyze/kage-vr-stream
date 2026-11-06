# KAGÉ VR Stream

Plateforme open source de streaming PCVR (Wi-Fi, et à terme USB-C) entre un PC Windows et un Meta Quest 3.

> **État : phase 1 (fondations).** Ce n'est pas encore un logiciel jouable. Il n'y a ni vidéo, ni intégration SteamVR, ni chiffrement. La liste exacte de ce qui fonctionne est dans [docs/STATUS.md](docs/STATUS.md).

## Structure

```
core/                    bibliothèque partagée C++20 (protocole, réseau, synchro, télémétrie)
apps/windows-host/       hôte Windows (CLI en phase 1)
apps/quest-client/       APK Quest native (OpenXR + GLES, Gradle/NDK)
tests/                   tests unitaires et d'intégration (CTest, sans dépendance externe)
scripts/                 scripts de compilation PowerShell
docs/                    architecture, état, compilation
```

## Démarrage rapide (hôte)

```powershell
.\scripts\build-host.ps1
.\build\apps\windows-host\Release\kage-host.exe probe      # diagnostic matériel réel
.\build\apps\windows-host\Release\kage-host.exe serve      # attend le casque sur le LAN
```

- [Décisions d'architecture](docs/ARCHITECTURE.md)
- [Compilation et installation](docs/BUILD.md)
- [État des fonctionnalités](docs/STATUS.md)

## Feuille de route

0. Étude ✔ (décisions résumées) · 1. Fondations ◐ (APK à compiler) · 2. Connexion sécurisée · 3. Premier flux vidéo · 4. Stéréo · 5. Tracking · 6. **Pilote SteamVR** · 7. Audio · 8. Optimisations · 9. Interface · 10. Distribution

## Licence

À définir par le propriétaire du projet (MIT ou Apache-2.0 suggérées, compatibles avec ALVR (MIT), OpenVR (BSD-3) et le loader OpenXR (Apache-2.0)).
