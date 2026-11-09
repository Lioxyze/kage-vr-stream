# État des fonctionnalités

Légende : ✅ fonctionnel et testé en réel · 🟡 écrit, non validé · ❌ non implémenté
Dernière mise à jour : 2026-11-06. Matériel de test : RX 6700 XT + **Meta Quest 3S**, PC et casque en Wi-Fi (Freebox, 5 GHz).

## Session PCVR (bout en bout)

| Fonction | État | Preuve |
|---|---|---|
| Lobby VR (panneau, liste des PC, choix 72/80/90/120 Hz, sol en grille) | ✅ | capture du casque |
| Connexion depuis le casque → lancement de SteamVR sur le PC | ✅ | journaux de l'hôte, du pilote et du casque |
| Pilote SteamVR : casque reconnu, direct mode, vsync 90 Hz | ✅ | vrserver « Using existing HMD kage.KAGE-HMD » |
| Manettes vues par SteamVR comme Touch (modèles Quest Plus) | ✅ | position/orientation calées sur le modèle SteamVR (openxr_grip) + 1 cm latéral, validé casque porté |
| Image SteamVR (dashboard, environnement) affichée dans le casque | ✅ | capture du casque |
| Mode 60 images/s (écran 120 Hz, reprojection du casque) + verrou Wi-Fi basse latence | ✅ | 2026-11-06 : 60 i/s stables, 54–150 Mbit/s, encodage 6,9 ms (BALANCED), décodage 6–8 ms ; pertes seulement au démarrage de SteamVR et lors de 2 pics Wi-Fi (RTT > 200 ms). Qualité perçue et position des manettes : à confirmer casque porté |
| Débit adaptatif AIMD | ✅ | 75–100 Mbit/s sur un Wi-Fi calme ; avec chiffrement, Wi-Fi plus chargé (transit 11–18 ms) : 55–99 Mbit/s, 17 images perdues en 88 s. Ignore les baisses de cadence de SteamVR ; le délai seul ne descend pas sous 50 Mbit/s |
| Boutons des manettes vers les jeux | 🟡 | envoyés et câblés au profil Touch, pas encore testés en jeu |
| Vibrations | 🟡 | chemin complet écrit, non testé |
| Mains (suivi des mains) | 🟡 | affichées dans le lobby, non vérifiées casque porté ; pas transmises aux jeux |
| Menu en session (appui long Menu gauche) : stats, Recentrer, Déconnecter | 🟡 | non testé casque porté |
| Calibration automatique de la prédiction de pose | 🟡 | active, non mesurée casque porté |
| Jeu PCVR réel testé | ✅ | VRChat à 60 i/s : qualité et fluidité jugées bonnes, petits gels occasionnels (perte Wi-Fi puis attente d'image clé) |
| Audio PC → casque | 🟡 | flux reçu et lecture AAudio démarrée dans le casque (journaux), son pas encore écouté casque porté (capture WASAPI de la sortie par défaut, PCM 16 bits brut ≈1,5 Mbit/s, AAudio basse latence, tampon 30 ms), pas encore testé dans le casque. Le son joue aussi sur le PC. Micro : non |
| Chiffrement et appairage | ✅ | testé avec le Quest 3S le 2026-11-06 : appairage par code accepté dans la GUI, session chiffrée établie, 90 images/s. Appairage par code à 6 chiffres comparé casque/PC ; échange de clés X25519 statiques + éphémères (authentification mutuelle, confidentialité persistante) ; XChaCha20-Poly1305 par paquet, anti-rejeu. Bibliothèque : Monocypher 4.0.2. Restent en clair : découverte et demande de lancement de SteamVR (sans données sensibles) |
| USB-C | ❌ | |
| Full body Rift CV1 (trackers pieds + bassin, calibration intégrée, lissage, maintien éveillé) | 🟡 | Rift lu par kage-host hors de SteamVR, poses en local vers le pilote ; testé : 2/3 trackers suivis et calibrés, visibles dans SteamVR avec KAGÉ ; à valider casque porté avec Virtual Desktop |
| Pack prêt à l'emploi dist/KageVR (scripts/package.ps1) | ✅ | KageVR.exe + kage-host + pilote + APK + LISEZMOI ; pilote installé depuis le pack et chargé par SteamVR ; nécessite le runtime .NET 9 Desktop |
| Full body Rift CV1 (trackers pieds + bassin, calibration intégrée, lissage, maintien éveillé) | 🟡 | Rift lu par kage-host hors de SteamVR, poses en local vers le pilote ; testé : 2/3 trackers suivis et calibrés, visibles dans SteamVR avec KAGÉ ; à valider casque porté avec Virtual Desktop |
| Pack prêt à l'emploi `distKageVR` (`scriptspackage.ps1`) | ✅ | KageVR.exe + kage-host + pilote + APK + LISEZMOI ; pilote installé depuis le pack et chargé par SteamVR. Nécessite le runtime .NET 9 Desktop |

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

## Wi-Fi : priorité WMM + FEC (test réel 2026-11-09)

- Priorité WMM vidéo (qWAVE AudioVideo côté PC, IP_TOS 0xB8 côté Quest) : `Wi-Fi priority on` dans driver.log.
- FEC XOR entrelacée, 1 parité pour 8 fragments, désactivée en USB. Le débit baisse tout de suite si le délai dépasse le meilleur de plus de 25 ms.
- Session Wi-Fi de 101 s à 60 i/s, démarrée à 150 Mbit/s : 0,72 image perdue/s, transit moyen 15,8 ms, pics > 20 ms sur 18 % des secondes, congestion sur 18 % des secondes.
- Avant ce changement, session de 34 s à 60 i/s plafonnée à 60 Mbit/s : 0,62 image perdue/s, transit 16,5 ms, pics sur 11 % des secondes.
- Les conditions ne sont pas les mêmes (débit ×2,5, durée différente), donc pas de comparaison stricte. L'utilisateur ressent beaucoup moins de saccades.
- Nombre de fragments récupérés par la FEC : Non disponible (logcat non lu, câble débranché).

## 120 i/s, Vue VR, menu (test réel 2026-11-09, câble USB)

- 120 i/s (Quest 120 Hz, HEVC 2832×1488) : 116 à 120 i/s, 0 image perdue, décodage 3,5 à 4,2 ms, rtt 2 à 8 ms. Le débit monte à 150 Mbit/s.
- Vue VR SteamVR : l'image s'affiche, en deux yeux côte à côte. Elle n'était pas noire, la fenêtre était juste réduite. `showMirrorView` est à true dans steamvr.vrsettings.
- Menu de session : un appui long sur ☰ (manette gauche) l'ouvre (`menu: overlay open` dans logcat). Autre geste : X+Y maintenus 0,6 s.
- Recentrer et netteté rapide : l'utilisateur dit que ça marche bien (aucune mesure).
- Sonde de débit Wi-Fi : Non disponible (session en USB, la sonde ne tourne qu'en Wi-Fi).

## Sonde de débit Wi-Fi au démarrage (test réel 2026-11-09)

- Sonde à 350 Mbit/s × 3000 paquets : 1401/3000 reçus en 256 ms, soit 60 Mbit/s mesurés et un départ à 36 Mbit/s. La salve a fait perdre 21 images au démarrage, avec un rtt de 114 ms.
- Sonde à 200 Mbit/s × 2000 paquets : 1220/2000 reçus en 387 ms, soit 34 Mbit/s mesurés et un départ à 20 Mbit/s. Aucune image perdue au démarrage.
- Les deux sessions, sans la sonde, se stabilisent entre 20 et 33 Mbit/s avec de la congestion : la mesure correspond donc au Wi-Fi de ce soir. Le PC est lui aussi en Wi-Fi, donc chaque image passe deux fois par les ondes.
- Le gain sur la latence ressentie n'est pas mesuré (Non disponible).

## Bouton « Mettre à jour » (test réel 2026-11-09)

- L'ancienne version 2026.11.09.1658, extraite dans un dossier à part, propose bien la 1952. Après « Oui », KageVR se ferme, les fichiers sont remplacés et KageVR redémarre tout seul. version.txt passe à 2026.11.09.1952 et driver_kage.dll est identique à celui de la release.
- L'APK du casque branché en USB est réinstallé (lastUpdateTime 20:02:32).
- Le SHA-256 est vérifié par le code avant de remplacer les fichiers (aucune erreur). Le cas d'un fichier corrompu n'a pas été testé.

## Latence d'encodage côté PC : banc `kage-encbench` (mesure réelle 2026-11-09, sans casque)

`build/drivers/steamvr/Release/kage-encbench.exe [charge]` : AMF HEVC 2832×1488 à 100 Mbit/s, tranches et rafraîchissement progressif comme le pilote, image de synthèse animée. On mesure le temps entre l'envoi d'une image à l'encodeur et la sortie de sa dernière tranche (médiane / p95).

| preset | 72 i/s | 90 i/s | 120 i/s |
|---|---|---|---|
| quality | 6,26 / 6,78 ms | 6,28 / 8,76 | 6,27 / 9,86 |
| balanced | 5,22 / 6,89 | 5,22 / 7,32 | 5,23 / 8,05 |
| speed | 5,12 / 7,68 | 5,09 / 5,84 | 5,12 / 7,24 |

- Changement : le preset « balanced » est maintenant utilisé à toutes les cadences (avant : « quality » à 72 i/s et moins). Gain mesuré : environ 1 ms d'encodage à 72 i/s et moins. Effet sur la qualité d'image : Non disponible (pas de mesure objective).
- Avec la carte graphique chargée par un faux jeu (`charge`) : 36 à 40 ms en médiane, contre 5,2 ms sans charge. `SetGPUThreadPriority(7)` et `D3DKMTSetProcessSchedulingPriorityClass` HIGH ou REALTIME sont acceptés mais ne changent rien (écart inférieur à 1 ms), donc ils ne sont pas appliqués au pilote.
- En jeu réel (driver.log, toute la journée) : temps « encode » moyen de 5,4 ms à 120 i/s, 7,3 ms à 90 i/s et 8 à 12 ms à 60 i/s. L'écart avec le banc vient probablement de l'occupation du GPU par le jeu (supposition, non mesurée séparément).

## Diagnostic Wi-Fi (kage-host probe, 2026-11-09)

- L'onglet Diagnostic affiche maintenant le SSID, la norme, la fréquence exacte, le canal, le signal et les débits négociés du Wi-Fi du PC, avec des alertes en cas de 2,4 GHz, de signal sous 60 % ou de débit négocié sous 400 Mbit/s.
- Mesure du PC : Wi-Fi 6 (802.11ax), 5,320 GHz, canal 64, signal 85 %, 2402 Mbit/s négociés. La liaison PC → box n'est donc pas le point faible : la limite d'environ 30 Mbit/s vue ce soir vient d'ailleurs (côté casque ou encombrement des ondes, Non disponible).
- Le pilote KAGÉ n'est plus signalé comme « autre pilote OpenVR ».

## Mise à jour : vérification au démarrage

- Au lancement, KageVR interroge l'API publique des releases et affiche « Mettre à jour ● » si une version plus récente existe. Il n'installe jamais rien tout seul. Testé avec version.txt = 1658 : la pastille s'affiche.
