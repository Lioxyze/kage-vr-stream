# Mode USB (câble) — plan, non implémenté

Constat : le protocole KAGÉ est en UDP ; `adb forward`/`adb reverse` ne transportent que du TCP.

Plan retenu (même principe que le mode filaire d'ALVR) :
1. Client Quest : option « USB » → se connecte à 127.0.0.1:47810 en TCP au lieu de chercher le PC en UDP.
2. `adb reverse tcp:47810 tcp:47810` lancé par KageVR.exe (adb fourni dans le dossier, détection du casque).
3. kage-host : écoute TCP 127.0.0.1:47810 et fait le pont TCP <-> UDP local (trames préfixées par leur longueur u16) vers les ports 47800-47802.
4. Le chiffrement reste identique (déjà au niveau paquet).
5. En USB : désactiver le pacing et l'AIMD perte (pas de pertes en TCP), fixer le débit à 150-200 Mbit/s.

Risque : le blocage en tête de file du TCP peut ajouter de la latence si le câble sature (USB 2 ≈ 300 Mbit/s réels).
Statut : Non disponible (rien de testé).