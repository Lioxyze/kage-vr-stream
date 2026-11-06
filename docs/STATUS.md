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
| Socket UDP Android | 🟡 | compilé uniquement quand l'APK l'est ; non testé sur le casque |
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
| Projet Gradle/NDK, manifeste Quest | 🟡 écrit, **pas encore compilé** (SDK Android absent) |
| Session OpenXR GLES, swapchains stéréo, couche de projection | 🟡 écrit, non compilé, non testé sur le casque |
| Journalisation de la résolution recommandée par œil et des fréquences supportées | 🟡 |
| Découverte du PC + ping / estimation d'horloge en arrière-plan | 🟡 |
| Affichage d'un texte ou d'un menu VR | ❌ (pour l'instant, une couleur unie indique l'état) |
| Décodage, tracking envoyé au PC, contrôleurs, audio | ❌ |

## Tests réellement exécutés (phase 1)

Résultats complets dans la sortie de `ctest` ; extrait :

- `test_protocol` : 11 tests OK (fuzz : 200 000 entrées aléatoires, 62 structurellement valides, aucun plantage)
- `test_video_fragment` : 7 tests OK
- `test_sync_telemetry` : 7 tests OK
- `test_loopback` : 2 tests OK — RTT en boucle locale mesuré : médiane 27 µs, p99 68 µs
- `kage-host ping` vers l'hôte local : 300/300 et 100/100 réponses ; médiane 0,13–0,14 ms. **C'est le même PC, ce n'est pas une mesure Wi-Fi.**
