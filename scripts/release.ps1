# Builds the portable package and publishes it as a GitHub release in the PUBLIC
# binaries-only repo Lioxyze/kage-vr-releases (the source repo stays private).
# KageVR's "Mettre à jour" button downloads the latest release from there.
# Usage: .\scripts\release.ps1   (needs gh logged in; builds must be up to date)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$repo = 'Lioxyze/kage-vr-releases'
$version = Get-Date -Format 'yyyy.MM.dd.HHmm'

& "$root\scripts\package.ps1" | Out-Null
dotnet publish "$root\apps\windows-gui\KageVR.Gui.csproj" -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o "$root\build\gui-portable" | Out-Null
if ($LASTEXITCODE) { throw 'dotnet publish a échoué' }

$o = "$root\dist\KageVR-Portable"
if (Test-Path $o) { Remove-Item $o -Recurse -Force }
Copy-Item -Recurse "$root\dist\KageVR" $o
Remove-Item "$o\KageVR.dll", "$o\KageVR.deps.json", "$o\KageVR.runtimeconfig.json", "$o\KageVR.pdb" -ErrorAction SilentlyContinue
Copy-Item "$root\build\gui-portable\KageVR.exe" $o -Force
foreach ($d in "$root\dist\KageVR", $o) { [IO.File]::WriteAllText("$d\version.txt", $version) }

$zip = "$root\dist\KageVR-Portable.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive "$o\*" $zip
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("$zip.sha256", "$sha  KageVR-Portable.zip")

if (-not ((gh repo list Lioxyze --limit 200 --json name -q '.[].name') -contains 'kage-vr-releases')) {
    gh repo create $repo --public --description 'KAGE VR Stream - versions prêtes à installer (KageVR.exe + APK Quest)' | Out-Null
}
gh release create "v$version" $zip "$zip.sha256" --repo $repo --title "KageVR $version" `
    --notes "Télécharger KageVR-Portable.zip, extraire, lancer KageVR.exe (voir LISEZMOI.txt). Mise à jour : bouton « Mettre à jour » dans KageVR."
if ($LASTEXITCODE) { throw 'gh release create a échoué' }
Write-Host "Publié : v$version ($repo)"
