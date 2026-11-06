# État des fonctionnalités

Légende : ✅ fonctionnel et testé en réel · 🟡 écrit, non validé · ❌ non implémenté
Dernière mise à jour : 2026-11-06. Matériel de test : RX 6700 XT + **Meta Quest 3S**, PC et casque en Wi-Fi (Freebox, 5 GHz).

## Session PCVR (bout en bout)

| Fonction | État | Preuve |
|---|---|---|
| Lobby VR (panneau, liste des PC, choix 72/80/90/120 Hz, sol en grille) | ✅ | capture du casque |
| Connexion depuis le casque → lancement de SteamVR sur le PC | ✅ | journaux de l'hôte, du pilote et du casque |
| Pilote SteamVR : casque reconnu, direct mode, vsync 90 Hz | ✅ | vrserver « Using existing HMD kage.KAGE-HMD » |
| Manettes vues par SteamVR comme Touch (modèles Quest Plus) avec leurs poses | ✅ | capture : manettes et rayon dans SteamVR |
| Image SteamVR (dashboard, environnement) affichée dans le casque | ✅ | capture du casque |
| Mode 60 images/s (écran 120 Hz, reprojection du casque) + verrou Wi-Fi basse latence | ✅ | 2026-11-06 : 60 i/s stables, 54–150 Mbit/s, encodage 6,9 ms (BALANCED), décodage 6–8 ms ; pertes seulement au démarrage de SteamVR et lors de 2 pics Wi-Fi (RTT > 200 ms). Qualité perçue et position des manettes : à confirmer casque porté |
| Débit adaptatif AIMD | ✅ | 75–100 Mbit/s sur un Wi-Fi calme ; avec chiffrement, Wi-Fi plus chargé (transit 11–18 ms) : 55–99 Mbit/s, 17 images perdues en 88 s. Ignore les baisses de cadence de SteamVR ; le délai seul ne descend pas sous 50 Mbit/s |
| Boutons des manettes vers les jeux | 🟡 | envoyés et câblés au profil Touch, pas encore testés en jeu |
| Vibrations | 🟡 | chemin complet écrit, non testé |
| Mains (suivi des mains) | 🟡 | affichées dans le lobby, non vérifiées casque porté ; pas transmises aux jeux |
| Menu en session (appui long Menu gauche) : stats, Recentrer, Déconnecter | 🟡 | non testé casque porté |
| Calibration automatique de la prédiction de pose | 🟡 | active, non mesurée casque porté |
| Jeu PCVR réel testé | ❌ | à faire, casque porté |
| Audio PC → casque | 🟡 | flux reçu et lecture AAudio démarrée dans le casque (journaux), son pas encore écouté casque porté (capture WASAPI de la sortie par défaut, PCM 16 bits brut ≈1,5 Mbit/s, AAudio basse latence, tampon 30 ms), pas encore testé dans le casque. Le son joue aussi sur le PC. Micro : non |
| Chiffrement et appairage | ✅ | testé avec le Quest 3S le 2026-11-06 : appairage par code accepté dans la GUI, session chiffrée établie, 90 images/s. Appairage par code à 6 chiffres comparé casque/PC ; échange de clés X25519 statiques + éphémères (authentification mutuelle, confidentialité persistante) ; XChaCha20-Poly1305 par paquet, anti-rejeu. Bibliothèque : Monocypher 4.0.2. Restent en clair : découverte et demande de lancement de SteamVR (sans données sensibles) |
| USB-C | ❌ | |

## Mesures réelles (session SteamVR, 90 Hz, HEVC 3360×1760)

| Mesure | Valeur |
|---|---|
| Images reçues | 90/s, 0 perte avec le débit adaptatif |
| Encodage AMF (soumission → sortie) | 6,6 à 7,1 ms |
| Réseau, aller simple (horloges synchronisées) | 4 à 9 ms (avant le débit adaptatif, à 100 Mbit/s fixe : 33 à 46 ms et 11 à 19 images perdues/s) |
| Décodage Quest | 3,7 à 6,4 ms |
| Somme encodage + réseau + décodage | environ 15 à 22 ms (**ce n'est pas le motion-to-photon**) |
| Aller-retour | 6 à 13 ms |
| CPU du PC | 24 % (interface) |
| Charge GPU | Non disponible (non mesurée) |

## Composants

- `core` : protocole, fragmentation, horloge, statistiques, UDP, chiffrement (`kage/crypto`). 5 suites de tests ✅.
- `kage-host` : probe, serve (découverte + lancement de SteamVR), install-driver/remove-driver ✅.
- `drivers/steamvr` : pilote OpenVR, composition D3D11, AMF HEVC/H.264 (H.264 non testé), débit adaptatif, `stats.json` ✅.
- `apps/windows-gui` : interface WPF (états, tableau de bord en direct, diagnostic, journal) ✅.
- `apps/quest-client` : OpenXR GLES, décodage MediaCodec sans copie, lobby, streaming ✅.

## Débogage

- `scripts/dev-e2e.ps1` : test de bout en bout automatique. Il utilise `adb setprop debug.kage.autoconnect 1` et désactive le capteur de proximité.
- `KAGE_DUMP_FRAME=1` : le pilote enregistre une image composée dans `%LOCALAPPDATA%\KageVR\frame.bmp`.
- Journaux : `%LOCALAPPDATA%\KageVR\driver.log`, `adb logcat -s KageVR`.
