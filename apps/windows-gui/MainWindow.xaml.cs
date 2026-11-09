using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading.Tasks;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace KageVR.Gui;

// The GUI is a thin front-end: the streaming engine lives in the SteamVR driver,
// discovery/launch in kage-host. Live numbers come from stats.json written by
// the driver; anything not measured is shown as "Non disponible".
public partial class MainWindow : Window
{
    private readonly string _dataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KageVR");
    private readonly string? _hostExe = FindHostExe();
    private Process? _service;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private readonly Dictionary<string, TextBlock> _values = new();
    private (ulong idle, ulong total)? _lastCpu;

    private static readonly (string key, string label)[] MetricDefs =
    {
        ("fps", "Images / s"), ("pipeline", "Latence pipeline estimée"), ("transit", "Réseau (aller simple)"), ("rtt", "Aller-retour"),
        ("encode", "Encodage"), ("decode", "Décodage"), ("bitrate", "Débit"), ("lost", "Images perdues / s"),
        ("codec", "Codec"), ("resolution", "Résolution du flux"), ("refresh", "Fréquence du casque"), ("cpu", "Charge CPU"),
        ("gpu", "Charge GPU"),
    };

    public MainWindow()
    {
        InitializeComponent();
        LoadSettingsUi();
        foreach (var (key, label) in MetricDefs)
        {
            var value = new TextBlock { Text = "Non disponible", FontSize = 22, FontWeight = FontWeights.SemiBold, Foreground = (Brush)FindResource("Cyan") };
            var card = new Border
            {
                Style = (Style)FindResource("CardStyle"),
                Child = new StackPanel { Children = { new TextBlock { Text = label.ToUpperInvariant(), FontSize = 11, Foreground = (Brush)FindResource("Dim") }, value } },
            };
            _values[key] = value;
            Metrics.Children.Add(card);
        }
        _timer.Tick += (_, _) => Refresh();
        _timer.Start();
        Closed += (_, _) => StopService();
        InitFullBody();
        CheckUpdateAtStartup();
        if (_hostExe == null) Log("kage-host.exe introuvable : compiler le projet (scripts\\build-host.ps1).");
        else StartService();  // the headset can find this PC as soon as the app is open
        Refresh();
    }

    private static string? FindHostExe()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
        {
            foreach (var c in new[] { Path.Combine(dir.FullName, "kage-host.exe"), Path.Combine(dir.FullName, "build", "apps", "windows-host", "Release", "kage-host.exe"),
                                      Path.Combine(dir.FullName, "apps", "windows-host", "Release", "kage-host.exe") })
                if (File.Exists(c)) return c;
        }
        return null;
    }

    private void Log(string line) => Dispatcher.Invoke(() =>
    {
        LogText.AppendText(line + Environment.NewLine);
        if (LogText.LineCount > 2000) LogText.Text = LogText.Text[^60000..];
        LogText.ScrollToEnd();
    });

    // ---- service ------------------------------------------------------------------
    private void StartService()
    {
        if (_hostExe == null || _service is { HasExited: false }) return;
        var psi = new ProcessStartInfo(_hostExe, "serve") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        _service = Process.Start(psi);
        if (_service == null) return;
        _service.ErrorDataReceived += (_, e) => { if (e.Data != null && !e.Data.Contains("discovery probe")) Log(e.Data); };
        _service.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            if (e.Data.StartsWith("PAIR_REQUEST ")) Dispatcher.BeginInvoke(() => OnPairRequest(e.Data));
            else Log(e.Data);
        };
        _service.BeginErrorReadLine();
        _service.BeginOutputReadLine();
    }

    private void StopService()
    {
        try { if (_service is { HasExited: false }) _service.Kill(); } catch { /* already gone */ }
        _service = null;
    }

    private void OnServiceClick(object s, RoutedEventArgs e)
    {
        if (_service is { HasExited: false }) StopService(); else StartService();
        Refresh();
    }

    private async Task<string> RunHost(string args)
    {
        if (_hostExe == null) return "kage-host.exe introuvable.";
        var psi = new ProcessStartInfo(_hostExe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                                                         StandardOutputEncoding = System.Text.Encoding.UTF8 };
        using var p = Process.Start(psi)!;
        var output = await p.StandardOutput.ReadToEndAsync();
        await p.WaitForExitAsync();
        return output;
    }

    private async void OnLaunchSteamVr(object s, RoutedEventArgs e) => Log((await RunHost("launch-steamvr")).Trim() is var o && o.Length > 0 ? o : "Lancement de SteamVR demandé.");

    private async void OnDriverClick(object s, RoutedEventArgs e)
    {
        var installed = DriverRegistered();
        if (!installed && MessageBox.Show("Enregistrer le pilote LIO VR (kage) dans SteamVR (vrpathreg adddriver) ?\nRéversible avec « Retirer le pilote ».",
                                          "Pilote SteamVR", MessageBoxButton.OKCancel) != MessageBoxResult.OK) return;
        if (installed && MessageBox.Show("Retirer le pilote LIO VR (kage) de SteamVR ?\nSans lui : plus d'image dans le casque ni de trackers Rift.",
                                         "Pilote SteamVR", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK) return;
        if (!installed) RemoveStaleDriverPaths();
        Log((await RunHost(installed ? "remove-driver" : "install-driver")).Trim());
        Refresh();
    }

    private async void OnDiagnostic(object s, RoutedEventArgs e)
    {
        DiagnosticText.Text = "Analyse en cours…";
        DiagnosticText.Text = await RunHost("probe");
        ((TabControl)DiagnosticText.Parent).SelectedIndex = 1;
    }

    // ---- pairing --------------------------------------------------------------------
    // "PAIR_REQUEST <code> <64 hex key> <name>" from kage-host. The user compares the
    // code with the one in the headset; only then is the key added to paired.txt.
    private readonly HashSet<string> _pairDialogs = new();
    private string PairedFile => Path.Combine(_dataDir, "paired.txt");

    private void OnPairRequest(string line)
    {
        var parts = line.Split(' ', 4);
        if (parts.Length < 3 || parts[1].Length != 6 || parts[2].Length != 64 || !_pairDialogs.Add(parts[2])) return;
        var name = parts.Length > 3 ? parts[3] : "Casque";
        var code = $"{parts[1][..3]} {parts[1][3..]}";
        Log($"Demande d'appairage de « {name} », code {code}.");
        var ok = MessageBox.Show(this, $"Le casque « {name} » demande à se connecter.\n\nCode : {code}\n\n" +
                                       "Accepter seulement si le casque affiche exactement le même code.",
                                 "Appairage LIO VR", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes;
        if (ok)
        {
            Directory.CreateDirectory(_dataDir);
            File.AppendAllText(PairedFile, $"{parts[2]} {name.Replace('\n', ' ')}\n");
            Log($"Casque « {name} » appairé.");
        }
        else Log($"Appairage de « {name} » refusé.");
        _pairDialogs.Remove(parts[2]);
    }

    private void OnForgetPaired(object s, RoutedEventArgs e)
    {
        if (!File.Exists(PairedFile)) { Log("Aucun casque appairé."); return; }
        if (MessageBox.Show(this, "Oublier tous les casques appairés ? Ils devront refaire l'appairage par code.", "Appairage",
                            MessageBoxButton.OKCancel) != MessageBoxResult.OK) return;
        File.Delete(PairedFile);
        Log("Casques appairés oubliés.");
    }

    // A KAGE driver registered from another folder (e.g. an older copy) would be loaded
    // twice by SteamVR: drop those entries from openvrpaths.vrpath before installing.
    private static void RemoveStaleDriverPaths()
    {
        var f = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr", "openvrpaths.vrpath");
        try
        {
            if (!File.Exists(f)) return;
            var root = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(f))!.AsObject();
            if (root["external_drivers"] is not System.Text.Json.Nodes.JsonArray arr) return;
            for (int i = arr.Count - 1; i >= 0; i--)
                if (arr[i]?.GetValue<string>() is { } p && p.TrimEnd('\\', '/').EndsWith(@"driver\kage", StringComparison.OrdinalIgnoreCase)) arr.RemoveAt(i);
            File.WriteAllText(f, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception) { /* vrpathreg will still add ours */ }
    }

    // ---- state ----------------------------------------------------------------------
    private static bool DriverRegistered()
    {
        var f = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr", "openvrpaths.vrpath");
        return File.Exists(f) && File.ReadAllText(f).Contains("driver\\\\kage", StringComparison.OrdinalIgnoreCase);
    }

    private void Set(TextBlock t, string text, string brush) { t.Text = text; t.Foreground = (Brush)FindResource(brush); }

    private void Refresh()
    {
        var running = _service is { HasExited: false };
        Set(ServiceState, running ? "Actif" : "Arrêté", running ? "Cyan" : "Warn");
        ServiceButton.Content = running ? "Arrêter le service" : "Démarrer le service";
        var steam = Process.GetProcessesByName("vrserver").Length > 0;
        Set(SteamVrState, steam ? "Lancé" : "Arrêté", steam ? "Cyan" : "Dim");
        var drv = DriverRegistered();
        Set(DriverState, drv ? "Installé" : "Non installé", drv ? "Cyan" : "Warn");
        DriverButton.Content = drv ? "Retirer le pilote SteamVR" : "Installer le pilote SteamVR";

        JsonElement? stats = null;
        try
        {
            var f = Path.Combine(_dataDir, "stats.json");
            if (File.Exists(f) && DateTime.Now - File.GetLastWriteTime(f) < TimeSpan.FromSeconds(3))
                stats = JsonDocument.Parse(File.ReadAllText(f)).RootElement.Clone();
        }
        catch (IOException) { /* being replaced, next tick */ }
        catch (JsonException) { }

        Set(HeadsetState, stats != null ? "En streaming" : "Non connecté", stats != null ? "Cyan" : "Dim");
        string Ms(string k) => stats?.TryGetProperty(k, out var v) == true && v.ValueKind == JsonValueKind.Number && v.GetDouble() > 0 ? $"{v.GetDouble():0.0} ms" : "Non disponible";
        string Num(string k, string unit = "") => stats?.TryGetProperty(k, out var v) == true && v.ValueKind == JsonValueKind.Number ? $"{v.GetDouble():0.#}{unit}" : "Non disponible";
        _values["fps"].Text = Num("fps");
        _values["transit"].Text = Ms("transitMs");
        _values["rtt"].Text = Ms("rttMs");
        _values["encode"].Text = Ms("encodeMs");
        _values["decode"].Text = Ms("decodeMs");
        _values["bitrate"].Text = Num("bitrateMbps", " Mbit/s");
        _values["lost"].Text = Num("lost");
        _values["codec"].Text = stats?.TryGetProperty("codec", out var c) == true ? c.GetString() ?? "Non disponible" : "Non disponible";
        _values["resolution"].Text = stats != null ? $"{stats.Value.GetProperty("width").GetInt32()}×{stats.Value.GetProperty("height").GetInt32()}" : "Non disponible";
        _values["refresh"].Text = Num("refreshHz", " Hz");
        // Sum of the measured stages only (encode + one-way network + decode): not motion-to-photon.
        if (stats != null && stats.Value.GetProperty("encodeMs").GetDouble() > 0 && stats.Value.GetProperty("decodeMs").GetDouble() > 0)
            _values["pipeline"].Text = $"{stats.Value.GetProperty("encodeMs").GetDouble() + stats.Value.GetProperty("transitMs").GetDouble() + stats.Value.GetProperty("decodeMs").GetDouble():0.0} ms";
        else
            _values["pipeline"].Text = "Non disponible";
        _values["cpu"].Text = CpuLoad() is double cpu ? $"{cpu:0} %" : "Non disponible";
        _values["gpu"].Text = "Non disponible";
        RefreshFullBody();
    }

    [DllImport("kernel32.dll")]
    private static extern bool GetSystemTimes(out long idle, out long kernel, out long user);

    private double? CpuLoad()
    {
        if (!GetSystemTimes(out var idle, out var kernel, out var user)) return null;
        var now = ((ulong)idle, (ulong)(kernel + user));  // kernel time includes idle
        var prev = _lastCpu;
        _lastCpu = now;
        if (prev == null || now.Item2 == prev.Value.total) return null;
        var dTotal = now.Item2 - prev.Value.total;
        var dIdle = now.Item1 - prev.Value.idle;
        return 100.0 * (dTotal - dIdle) / dTotal;
    }
}
