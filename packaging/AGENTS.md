# KAGÉ VR Stream — guide pour un agent IA (dépannage chez l'utilisateur)

Tu aides quelqu'un à faire marcher **KAGÉ VR Stream** : un streaming PCVR **PC Windows → Meta Quest**
(Wi-Fi ou câble USB), sans compte, tout en local. Réponds en **français**, simplement : l'utilisateur
n'est pas développeur. N'invente jamais de résultat : vérifie (journaux, commandes) avant d'affirmer.

## Règles de sécurité (obligatoires)
- Ne désactive jamais Windows Defender, le pare-feu ou les protections Android.
- Demande l'accord avant toute installation, modification système ou suppression.
- Ne touche pas aux clés : `host.key`, `paired.txt` (PC), `client.key`, `hosts.txt` (casque).
  Pour ré-appairer : bouton « Oublier les casques appairés » dans KageVR.
- Ne désactive jamais le pilote SteamVR `oculus` (Virtual Desktop en dépend).

## Ce dossier
- `KageVR.exe` : application PC (portable, aucun .NET à installer). Lance `kage-host.exe` (service)
  et installe le pilote SteamVR `driver\kage` (bouton « Installer le pilote SteamVR »).
- `KageVR-Quest.apk` : application du casque. Installation : `adb\adb.exe install -r KageVR-Quest.apk`
  (casque en mode développeur, débogage USB autorisé dans le casque).
- `adb\` : outils Android (platform-tools).

## Fonctionnement
1. KageVR.exe ouvert sur le PC → service actif (ports UDP 47800 découverte, 47801 contrôle, 47802 vidéo).
2. Dans le casque : app « KAGE VR Stream » (Sources inconnues) → choisir le PC
   (« (USB) » = câble via `adb reverse tcp:47810`, fait automatiquement par kage-host).
3. 1re fois : code à 6 chiffres affiché dans le casque, à confirmer sur le PC (appairage).
4. KAGÉ lance SteamVR tout seul. **Lancer KAGÉ dans le casque AVANT SteamVR** (sinon handshake raté :
   fermer SteamVR et reconnecter).

## Fichiers utiles (`%LOCALAPPDATA%\KageVR\`)
- `driver.log` : journal du pilote SteamVR (encodeur choisi, fps, pertes, débit, micro). **À lire en premier.**
  Lignes `encoder: ...`, `client: N fps, X lost, decode, transit, rtt -> débit`, `video: fps, encode ms`.
- `settings.ini` (onglet Réglages) : `encoder=auto|amf|gpu|software`, `codec=auto|h264`, `maxbitrate=<Mbit, 0=casque>`.
  Pris en compte au prochain lancement de SteamVR.
- `mic_device.txt` : nom (partiel) de la sortie Windows où jouer le micro du casque (défaut : VB-CABLE « CABLE Input »).
- Journal SteamVR : `C:\Program Files (x86)\Steam\logs\vrserver.txt` (chercher `kage`).
- Journal casque : `adb\adb.exe logcat -d -s KageVR:*`

## Pannes connues → solution
- **Écran noir / pas d'image, `encoder: none available`** : onglet Réglages → codec **H.264**, encodeur
  « Carte graphique via Windows » ; si toujours rien → « Processeur ». Fermer SteamVR, reconnecter.
- **Saccades / PC faible** : SteamVR → Paramètres → Vidéo → baisser la résolution ; dans le casque
  45 ou 60 i/s et qualité Basse/Moyenne ; Réglages → H.264.
- **Pertes (`lost` > 0) en Wi-Fi** : Wi-Fi 5 GHz, PC en Ethernet de préférence, ou câble USB (« (USB) »).
- **« the PC did not prove its paired key »** (logcat) : SteamVR lancé avant le casque → fermer SteamVR,
  relancer l'app du casque.
- **« Driver 'kage' not allowed to add tracked device KAGE-HMD »** (vrserver.txt) : un autre casque
  (Rift, Virtual Desktop) prend la place. SteamVR fermé, mettre `"forcedDriver": "kage"` dans la section
  `steamvr` de `C:\Program Files (x86)\Steam\config\steamvr.vrsettings` (sauvegarder le fichier avant ;
  à retirer pour réutiliser Virtual Desktop).
- **« Not loading driver kage because it is disabled »** : dans `steamvr.vrsettings`, `driver_kage.enable = true`.
- **Le casque ne voit pas le PC** : KageVR.exe ouvert ? même réseau ? autoriser `kage-host` sur les
  réseaux **privés** si Windows le demande (ne rien ouvrir d'autre).
- **Pas de son** : volume du casque (boutons du casque). **Micro** : installer VB-CABLE avec Installer-Micro-VBCABLE.bat (avec accord ; détails dans MICRO-LISEZMOI.txt), bouton « Micro » dans le menu KAGÉ du casque (appui long sur Menu) ; micro Windows = « CABLE Output ».
- **Boutons de volume du casque** : gérés par l'app KAGÉ (volume média).

## Diagnostic rapide (PowerShell)
```powershell
Get-Content "$env:LOCALAPPDATA\KageVR\driver.log" -Tail 30
Get-Content "C:\Program Files (x86)\Steam\logs\vrserver.txt" -Tail 400 | Select-String kage
.\adb\adb.exe devices; .\adb\adb.exe logcat -d -s KageVR:* | Select-Object -Last 40
Get-CimInstance Win32_VideoController | Select Name, DriverVersion
```
Mettre à jour le pilote graphique (site AMD/NVIDIA/Intel) résout souvent les échecs d'encodeur.
