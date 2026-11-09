// "Mettre à jour" button: downloads the latest KageVR-Portable.zip from the public
// releases repo (binaries only; the source repo is private), checks its SHA-256, then a
// small script swaps the files once KageVR has exited, reinstalls the headset APK when the
// headset is plugged in (adb), and restarts KageVR. User data (%LOCALAPPDATA%\KageVR:
// keys, pairing, settings) is never touched.
using System;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;

namespace KageVR.Gui;

public partial class MainWindow
{
    const string ReleasesApi = "https://api.github.com/repos/Lioxyze/kage-vr-releases/releases/latest";
    static string AppDir => AppContext.BaseDirectory;

    static string LocalVersion()
    {
        try { return File.ReadAllText(Path.Combine(AppDir, "version.txt")).Trim(); } catch { return "0"; }
    }

    // At launch: only tells that a newer version exists (button label + log), never installs.
    // One anonymous GET to the public releases API; silent when offline.
    async void CheckUpdateAtStartup()
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("KageVR-Updater");
            using var doc = JsonDocument.Parse(await http.GetStringAsync(ReleasesApi));
            var remote = (doc.RootElement.GetProperty("tag_name").GetString() ?? "").TrimStart('v');
            if (string.CompareOrdinal(remote, LocalVersion()) <= 0) return;
            UpdateButton.Content = "Mettre à jour ●";
            UpdateButton.ToolTip = $"Nouvelle version {remote} disponible (actuelle : {LocalVersion()})";
            Log($"Nouvelle version {remote} disponible : bouton « Mettre à jour » (actuelle : {LocalVersion()}).");
        }
        catch { /* offline or GitHub unreachable: the button still works by hand */ }
    }

    async void OnUpdate(object sender, RoutedEventArgs e)
    {
        try
        {
            using var http = new HttpClient();
            http.DefaultRequestHeaders.UserAgent.ParseAdd("KageVR-Updater");
            Log("Mise à jour : recherche…");
            using var doc = JsonDocument.Parse(await http.GetStringAsync(ReleasesApi));
            var tag = doc.RootElement.GetProperty("tag_name").GetString() ?? "";
            var remote = tag.TrimStart('v');
            if (string.CompareOrdinal(remote, LocalVersion()) <= 0)
            {
                MessageBox.Show(this, $"KageVR est à jour (version {LocalVersion()}).", "Mise à jour");
                return;
            }
            string? zipUrl = null, shaUrl = null;
            foreach (var a in doc.RootElement.GetProperty("assets").EnumerateArray())
            {
                var n = a.GetProperty("name").GetString();
                var u = a.GetProperty("browser_download_url").GetString();
                if (n == "KageVR-Portable.zip") zipUrl = u;
                else if (n == "KageVR-Portable.zip.sha256") shaUrl = u;
            }
            if (zipUrl == null || shaUrl == null) throw new Exception("fichiers de mise à jour absents de la version " + tag);
            if (MessageBox.Show(this, $"Nouvelle version {remote} disponible (actuelle : {LocalVersion()}).\n\n" +
                                      "KageVR et SteamVR vont se fermer, puis KageVR redémarrera tout seul.\n" +
                                      "Si le casque est branché en USB, son application sera aussi mise à jour.\n\nInstaller ?",
                                "Mise à jour", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;

            var tmp = Path.Combine(Path.GetTempPath(), "KageVR-update");
            if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
            Directory.CreateDirectory(tmp);
            var zip = Path.Combine(tmp, "KageVR-Portable.zip");
            Log("Mise à jour : téléchargement…");
            await File.WriteAllBytesAsync(zip, await http.GetByteArrayAsync(zipUrl));
            var expected = (await http.GetStringAsync(shaUrl)).Trim().Split(' ')[0].ToLowerInvariant();
            string actual;
            await using (var fs = File.OpenRead(zip)) actual = Convert.ToHexString(await SHA256.HashDataAsync(fs)).ToLowerInvariant();
            if (actual != expected) throw new Exception("fichier téléchargé corrompu (SHA-256 différent) : mise à jour annulée");
            var files = Path.Combine(tmp, "files");
            await Task.Run(() => System.IO.Compression.ZipFile.ExtractToDirectory(zip, files));

            // The swap runs after this process exits (its exe and the driver DLL are locked while running).
            var dir = AppDir.TrimEnd('\\').Replace("'", "''");  // single-quoted PowerShell paths
            var filesQ = files.Replace("'", "''");
            var script = Path.Combine(tmp, "apply.ps1");
            File.WriteAllText(script, $$"""
                $ErrorActionPreference = 'SilentlyContinue'
                Wait-Process -Id {{Environment.ProcessId}} -Timeout 30
                Get-Process kage-host,vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup | Stop-Process -Force
                Start-Sleep 2
                Copy-Item -Path '{{filesQ}}\*' -Destination '{{dir}}' -Recurse -Force
                $adb = Join-Path '{{dir}}' 'adb\adb.exe'
                if ((& $adb devices) -match "`tdevice") { & $adb install -r (Join-Path '{{dir}}' 'KageVR-Quest.apk') }
                Start-Process (Join-Path '{{dir}}' 'KageVR.exe')
                """);
            Process.Start(new ProcessStartInfo("powershell.exe", $"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"{script}\"")
                { UseShellExecute = false, CreateNoWindow = true });
            StopService();
            Application.Current.Shutdown();
        }
        catch (Exception ex)
        {
            Log("Mise à jour impossible : " + ex.Message);
            MessageBox.Show(this, "Mise à jour impossible : " + ex.Message, "Mise à jour");
        }
    }
}
