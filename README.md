# KAGÉ VR Stream

Plateforme open source de streaming PCVR (Wi-Fi, et à terme USB-C) entre un PC Windows et un Meta Quest 3.

> **État : première session PCVR fonctionnelle.** SteamVR s'affiche dans un Quest 3S en Wi-Fi à 90 Hz, avec les manettes suivies et un débit adaptatif. Il manque encore l'audio, le chiffrement et un test en jeu réel. Détails et mesures : [docs/STATUS.md](docs/STATUS.md).

## Utilisation

1. Compiler : `.\scripts\build-host.ps1`, `dotnet build apps\windows-gui\KageVR.Gui.csproj -c Release -o build\gui`, puis `.\scripts\build-quest.ps1 -Install`.
2. Lancer `build\gui\KageVR.exe`, puis cliquer sur **Installer le pilote SteamVR** (une seule fois).
3. Dans le casque : *Bibliothèque > Sources inconnues > KAGÉ VR Stream*, puis viser ton PC avec la manette et appuyer sur la gâchette.
4. SteamVR se lance tout seul sur le PC. Au premier lancement, faire **Configuration de la pièce > Debout uniquement**.
5. Première connexion : le casque affiche un code à 6 chiffres, le PC ouvre une fenêtre avec un code. Accepter seulement s'ils sont identiques.
6. Pendant la session : **appui long sur le bouton Menu gauche** pour afficher les statistiques ou se déconnecter.

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
