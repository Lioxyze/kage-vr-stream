# État des fonctionnalités

Légende : ✅ fonctionnel et testé · 🟡 écrit mais non validé · ❌ non implémenté

Dernière mise à jour : 2026-11-06, phase 1.

## Core (`kage_core`)

| Fonction | État | Preuve |
|---|---|---|
| Sérialisation little-endian bornée | ✅ | `test_protocol` |
| En-tête de paquet + validation | ✅ | `test_protocol` |
| Messages (découverte, ping/pong, heartbeat, déconnexion, tracking) | ✅ | `test_protocol` (aller-retour, rejet des valeurs invalides, fuzz) |
| Fragmentation / réassemblage vidéo (abandon des images obsolètes, mémoire bornée) | ✅ | `test_video_fragment` |
| Estimation de décalage d'horloge (filtre RTT minimal) | ✅ | `test_sync_telemetry` |
| Statistiques, gigue RFC 3550, comptage de pertes | ✅ | `test_sync_telemetry` |
| Socket UDP Windows | ✅ | `test_loopback` (vrais sockets) |
| Socket UDP Android | 🟡 | compilé (NDK 27.2, clang, arm64) ; non testé sur le casque |
| Service hôte (réponse découverte + pong) / client (découverte + ping) | ✅ sur PC | `test_loopback`, `kage-host serve`/`discover`/`ping` |
| Chiffrement, appairage, authentification | ❌ | phase 2 |
| Interface `Transport` (UDP / ADB / USB AOA) | ❌ | phase 2 et suivantes |

## Hôte Windows

| Fonction | État |
|---|---|
| `probe` : OS, CPU, RAM, GPU (DXGI) + version du pilote, encodeurs matériels H.264/HEVC/AV1 (Media Foundation), version du runtime AMF, SteamVR, pilotes OpenVR tiers, runtime OpenXR actif, cartes réseau, observations | ✅ exécuté sur ce PC |
| `probe --json` | ✅ (JSON vérifié avec `ConvertFrom-Json`) |
| `serve` / `discover` / `ping` | ✅ sur le PC (boucle locale + IP locale) ; ❌ pas encore testé depuis le Quest |
| Interrogation des capacités de l'encodeur AMF (résolutions, profils, mode faible latence) | ❌ phase 3 |
| Encodage, capture, pilote SteamVR, audio, interface graphique | ❌ |

## Client Quest

| Fonction | État |
|---|---|
| Projet Gradle/NDK, manifeste Quest | ✅ compilé : `app-debug.apk` 6,2 Mo, arm64-v8a uniquement, minSdk 29 / targetSdk 32, 3 permissions (INTERNET, OPENXR, OPENXR_SYSTEM), sans avertissement de compilation |
| Session OpenXR GLES, swapchains stéréo, couche de projection | ✅ sur Quest 3S : la session atteint FOCUSED, les images sont soumises |
| Journalisation de la résolution recommandée par œil et des fréquences supportées | ✅ (valeurs ci-dessous) |
| Découverte du PC + ping / estimation d'horloge en arrière-plan | ✅ en Wi-Fi réel (mesures ci-dessous) |
| Affichage d'un texte ou d'un menu VR | ❌ (pour l'instant, une couleur unie indique l'état) |
| Décodage, tracking envoyé au PC, contrôleurs, audio | ❌ |

## Premier test sur casque (2026-11-06)

Matériel : **Meta Quest 3S** (`panther`, Android 14 / API 34, build UP1A.231005.007.A1) et non Quest 3. PC en Wi-Fi (192.168.1.10), casque en Wi-Fi 5 GHz, canal 64 (192.168.1.100), même routeur (Freebox).

Valeurs rapportées par le runtime (logcat `KageVR`) :
- Runtime OpenXR « Oculus », système « Meta Quest 3S », 75 extensions d'instance
- GPU Adreno 740, OpenGL ES 3.2
- Résolution recommandée : **1680×1760 par œil** (maximum de swapchain 8192×8192)
- Fréquences : **72 / 80 / 90 / 120 Hz** (72 Hz par défaut à l'ouverture de la session)

Liaison UDP KAGÉ, mesurée par le client (RTT = aller-retour PC↔casque, hors traitement) :

| Mode | RTT médian | p99 | Min | Pertes |
|---|---|---|---|---|
| Maintien, 1 ping / 500 ms | 21–35 ms | 40–52 ms | — | 8/368 sans réponse (~2 %) |
| Rafale, 1 ping / 5 ms (3 × 100) | 7,7–8,0 ms | 16,7–41,3 ms | 5,1 ms | 0/300 |

Interprétation : à faible trafic, la mise en veille Wi-Fi ajoute environ 15 à 25 ms ; le trafic continu (streaming) la supprime. Le plancher d'environ 5 ms et la queue p99 sont cohérents avec deux sauts Wi-Fi (PC et casque sur le même point d'accès) : relier le PC en Ethernet est la première amélioration attendue, **à confirmer par une mesure**.

## Tests réellement exécutés (phase 1)

Résultats complets dans la sortie de `ctest` ; extrait :

- `test_protocol` : 11 tests OK (fuzz : 200 000 entrées aléatoires, 62 structurellement valides, aucun plantage)
- `test_video_fragment` : 7 tests OK
- `test_sync_telemetry` : 7 tests OK
- `test_loopback` : 2 tests OK — RTT en boucle locale mesuré : médiane 27 µs, p99 68 µs
- `kage-host ping` vers l'hôte local : 300/300 et 100/100 réponses ; médiane 0,13–0,14 ms. **C'est le même PC, ce n'est pas une mesure Wi-Fi.**
