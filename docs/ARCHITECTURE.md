# LIO VR (KAGÉ VR Stream) — décisions d'architecture

Document volontairement court : il fixe les choix pris et leurs raisons. Il ne remplace pas une étude complète.

## Composants

| Composant | Langage / outils | Rôle |
|---|---|---|
| `core/` (`kage_core`) | C++20, CMake, sans dépendance | Protocole, validation, fragmentation vidéo, UDP, synchro d'horloge, télémétrie. Compilé pour Windows x64 **et** Android arm64. |
| `apps/windows-host/` | C++20, Win32, DXGI, Media Foundation | Phase 1 : outil en ligne de commande (`probe`, `serve`, `discover`, `ping`). Deviendra le service/moteur de l'hôte. |
| `apps/quest-client/` | C++20 NDK, OpenXR (loader Khronos), GLES 3, Gradle/AGP | APK native (NativeActivity, sans code Java). |
| *(phase 6)* `modules/steamvr-driver/` | C++, `openvr_driver.h` (BSD-3, Valve) | Pilote OpenVR chargé par `vrserver.exe`. |
| *(phase 9)* interface Windows | C# / WPF (.NET, déjà installé) | Interface graphique séparée du moteur, branchée via une API C ou de l'IPC locale. |

## Décisions clés

1. **Intégration PCVR : un pilote SteamVR (OpenVR driver)**, sur le modèle d'ALVR et de Virtual Desktop. Le pilote déclare un HMD (`ITrackedDeviceServerDriver`) et deux contrôleurs, reçoit les images composées de chaque œil via `IVRDriverDirectModeComponent` (textures D3D11 partagées) et injecte les poses reçues du casque. Les jeux OpenXR passent par SteamVR seulement si SteamVR est le runtime OpenXR actif. Le `probe` détecte ce point : sur ce PC, le runtime actif est actuellement celui de Virtual Desktop. Écrire un runtime OpenXR maison n'est pas prévu pour la v1 (périmètre trop grand).
2. **Interface Windows en WPF/.NET plutôt qu'en Qt 6** : le SDK .NET 9 est déjà présent, il n'y a pas de contrainte LGPL, et l'interface reste découplée du moteur C++. Qt reste possible plus tard, car le moteur ne dépend d'aucune interface.
3. **Encodage : AMD AMF en direct** pour la RX 6700 XT, avec H.264 et HEVC. L'AV1 matériel n'est pas disponible sur RDNA2 : le `probe` le confirme, aucun MFT AV1 matériel n'est présent. Media Foundation sert uniquement à détecter les capacités. NVENC et Quick Sync viendront derrière la même interface `VideoEncoder`.
4. **Décodage Quest : `AMediaCodec` (NDK)** vers une `ANativeWindow`/`SurfaceTexture`, puis présentation en couche de projection OpenXR. La reprojection reste celle du runtime Quest (ATW/ASW) : on ne réimplémente pas de timewarp.
5. **Transport : UDP natif**, avec un en-tête commun de 24 octets ([packet.hpp](../core/include/kage/protocol/packet.hpp)) et des canaux séparés (découverte, contrôle, tracking, vidéo, audio).
   - Vidéo : images fragmentées sous 1 400 octets par datagramme, sans fragmentation IP. Une image incomplète est **abandonnée** dès qu'une image plus récente est complète, puis une image clé est demandée. Aucune retransmission vidéo : elle coûterait au moins un RTT complet. Le FEC est évalué en phase 8, mesures à l'appui.
   - Tracking : petits paquets envoyés à chaque frame, sans retransmission, le plus récent l'emporte.
   - Contrôle : messages idempotents ou acquittés (phase 2).
   - QUIC est écarté pour la vidéo : le contrôle de congestion et l'ordonnancement fiable ajoutent de la latence. Il pourra servir pour le canal de contrôle si besoin.
6. **Sécurité (phase 2)** : appairage par code à 6 chiffres affiché sur le PC et saisi dans le casque. Échange de clés X25519 authentifié, chiffrement AEAD ChaCha20-Poly1305 ou AES-GCM avec une bibliothèque reconnue (libsodium ou Monocypher, choix au début de la phase 2, aucun algorithme maison). L'en-tête de paquet sert de donnée associée. Les clés publiques des casques sont stockées côté PC et révocables.
7. **Synchronisation** : échange NTP à 4 horodatages avec filtre sur le RTT minimal ([clock_sync.hpp](../core/include/kage/sync/clock_sync.hpp)). Toutes les poses sont horodatées avec l'horloge du casque puis converties.
8. **Validation stricte** : chaque décodeur vérifie les longueurs, les flottants finis, les quaternions unitaires et les énumérations, et refuse les octets en trop. Un fuzz de 200 000 entrées aléatoires fait partie des tests.

## Mode filaire USB-C (exigence 8 bis) — analyse préliminaire, à valider

- **`adb forward`/`adb reverse` ne transportent que du TCP**, pas d'UDP. Pour le développement, un tunnel ADB est donc utilisable avec un transport en flux (TCP). C'est pratique pour le débogage, mais pas idéal pour la vidéo, à cause du blocage en tête de file. C'est une piste à mesurer.
- **Partage de connexion USB (RNDIS/NCM)** : il donnerait un vrai lien IP sur lequel le transport UDP fonctionne sans modification. La disponibilité de ce mode sur Horizon OS reste **à vérifier sur le casque**.
- **Android Open Accessory (AOA)** : c'est un vrai transport USB natif. L'APK le reçoit via `UsbAccessory`, et le PC l'utilise via WinUSB/libusb. C'est la voie candidate pour une version distribuée.
- Conséquence sur l'architecture : le moteur passe par une interface `Transport` (envoi et réception de datagrammes avec un MTU). UDP en est la première implémentation ; ADB-TCP et AOA s'y ajouteront sans toucher au protocole. Le mode hybride bascule entre transports au niveau de la session.

## Risques identifiés

| Risque | Impact | Mitigation |
|---|---|---|
| Conflit avec le pilote OpenVR de Virtual Desktop déjà enregistré | SteamVR peut choisir le mauvais casque | Le `probe` le signale ; il faudra le désactiver dans SteamVR pendant les tests KAGÉ |
| Runtime OpenXR système = Virtual Desktop | Les jeux OpenXR natifs ne passent pas par SteamVR | Le `probe` le signale ; l'utilisateur doit choisir SteamVR comme runtime OpenXR |
| PC uniquement en Wi-Fi | Le Wi-Fi est partagé entre l'aller et le retour, d'où plus de gigue et de pertes | Recommandation Ethernet ; débit adaptatif (phase 8) |
| Pare-feu Windows | Le Quest ne joint pas le PC | Windows proposera d'autoriser `kage-host` sur les réseaux **privés** ; aucune règle n'est créée automatiquement |
| Limites du décodeur Quest 3 (résolution et débit HEVC) | Échec ou latence de décodage | Interroger `MediaCodecInfo` sur le casque (phase 3) ; aucune valeur n'est supposée |
