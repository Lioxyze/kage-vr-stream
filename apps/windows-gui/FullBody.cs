using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Win32;

namespace KageVR.Gui;

// Rift CV1 full body: the tracking itself runs in the SteamVR driver (LibOVR);
// this side handles the Meta software around it (launch, keep-awake, settings).
public partial class MainWindow
{
    private string FullBodyFlag => Path.Combine(_dataDir, "fullbody.enabled");
    private string KeepAwakeFlag => Path.Combine(_dataDir, "keepawake.enabled");
    private readonly DispatcherTimer _keepAwake = new() { Interval = TimeSpan.FromMinutes(9) };

    private static string? OculusSupportDir()
    {
        foreach (var d in new[] { @"C:\Program Files\Meta Horizon\Support", @"C:\Program Files\Oculus\Support" })
            if (Directory.Exists(d)) return d;
        return null;
    }

    private void InitFullBody()
    {
        FullBodyCheck.IsChecked = File.Exists(FullBodyFlag);
        // Remembered: keep the Rift awake from launch (else it sleeps and the trackers vanish).
        if (File.Exists(KeepAwakeFlag)) { KeepAwakeCheck.IsChecked = true; OnKeepAwakeToggle(KeepAwakeCheck, new RoutedEventArgs()); }
        _keepAwake.Tick += (_, _) => Task.Run(ToggleProximityBypass);
        Closed += (_, _) => { if (KeepAwakeCheck.IsChecked == true) StopKeepAwake(); };
    }

    private void RefreshFullBody()
    {
        var f = Path.Combine(_dataDir, "fullbody_status.txt");
        try { FullBodyStatus.Text = File.Exists(f) ? File.ReadAllText(f) : (File.Exists(FullBodyFlag) ? "Activé : lancer SteamVR" : "Désactivé"); }
        catch (IOException) { }
    }

    private void OnFullBodyToggle(object s, RoutedEventArgs e)
    {
        Directory.CreateDirectory(_dataDir);
        if (FullBodyCheck.IsChecked == true) File.WriteAllText(FullBodyFlag, "1");
        else File.Delete(FullBodyFlag);
        // kage-host starts/stops its Rift reader at launch: restart it to apply.
        StopService();
        StartService();
        Log(FullBodyCheck.IsChecked == true ? "Full body activé : lecture du Rift lancée. Relancer SteamVR pour voir les trackers." : "Full body désactivé (relancer SteamVR).");
    }

    private void OnCalibrate(object s, RoutedEventArgs e)
    {
        Directory.CreateDirectory(_dataDir);
        File.WriteAllText(Path.Combine(_dataDir, "calibrate.request"), "1");
        Log("Calibration demandée : colle la manette droite du Rift à celle du Quest et fais-les tourner 15 s.");
    }

    private void OnLaunchOculus(object s, RoutedEventArgs e)
    {
        var exe = OculusSupportDir() is { } d ? Path.Combine(d, "oculus-client", "OculusClient.exe") : null;
        if (exe == null || !File.Exists(exe)) { Log("Meta Horizon Link introuvable : l'installer depuis meta.com."); return; }
        Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
        Log("Meta Horizon Link lancé. Brancher le Rift CV1 et vérifier que les manettes (et l'objet VR) sont actives.");
    }

    private async Task<string> RunOdtCli(string commands)
    {
        var exe = OculusSupportDir() is { } d ? Path.Combine(d, "oculus-diagnostics", "OculusDebugToolCLI.exe") : null;
        if (exe == null || !File.Exists(exe)) return "OculusDebugToolCLI introuvable.";
        var tmp = Path.Combine(Path.GetTempPath(), "kage_odt.txt");
        await File.WriteAllTextAsync(tmp, commands + "\nexit\n");
        var psi = new ProcessStartInfo(exe, $"-f \"{tmp}\"") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        using var p = Process.Start(psi)!;
        var o = await p.StandardOutput.ReadToEndAsync();
        await p.WaitForExitAsync();
        return o;
    }

    private async void OnTouchNoSleep(object s, RoutedEventArgs e)
    {
        await RunOdtCli("server: Touch.DisableSleep true");
        Log("Veille des manettes Touch désactivée (effectif après avoir retiré/remis les piles ou redémarré Meta Horizon Link). Les piles s'usent plus vite.");
    }

    // SteamVR's own Oculus drivers would add the Rift as a second headset (and render
    // to it): disable them, KAGE reads the Rift directly through LibOVR.
    private void OnPrepareSteamVr(object s, RoutedEventArgs e)
    {
        if (Process.GetProcessesByName("vrserver").Length > 0) { MessageBox.Show(this, "Fermer SteamVR d'abord (il réécrit ses réglages en quittant)."); return; }
        var steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string ?? @"C:\Program Files (x86)\Steam";
        var file = Path.Combine(steam.Replace('/', '\\'), "config", "steamvr.vrsettings");
        if (MessageBox.Show(this, $"Désactiver les pilotes Oculus intégrés à SteamVR dans\n{file} ?\n\nLe Rift ne sera plus vu comme un 2e casque (moins de charge). Une copie de sauvegarde est faite (.kage-backup).",
                            "Préparer SteamVR", MessageBoxButton.OKCancel) != MessageBoxResult.OK) return;
        try
        {
            var root = File.Exists(file) ? JsonNode.Parse(File.ReadAllText(file))!.AsObject() : new JsonObject();
            if (File.Exists(file) && !File.Exists(file + ".kage-backup")) File.Copy(file, file + ".kage-backup");
            foreach (var drv in new[] { "driver_oculus", "driver_oculus_legacy" })
            {
                if (root[drv] is not JsonObject o) root[drv] = o = new JsonObject();
                o["enable"] = false;
            }
            File.WriteAllText(file, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            Log("SteamVR préparé : pilotes Oculus intégrés désactivés.");
        }
        catch (Exception ex) { Log("Échec de la modification de steamvr.vrsettings : " + ex.Message); }
    }

    // ---- keep-awake (same method as ODTKRA) ---------------------------------------
    // The Rift sleeps when its proximity sensor sees nobody, which freezes the Touch
    // tracking. The Oculus Debug Tool "Bypass Proximity Sensor Check" lasts ~10 min,
    // so it is toggled every 9 min. Rendering on the Rift is cut to 1 % resolution.
    private async void OnKeepAwakeToggle(object s, RoutedEventArgs e)
    {
        Directory.CreateDirectory(_dataDir);
        if (KeepAwakeCheck.IsChecked == true) File.WriteAllText(KeepAwakeFlag, "1"); else File.Delete(KeepAwakeFlag);
        if (KeepAwakeCheck.IsChecked == true)
        {
            await RunOdtCli("service set-pixels-per-display-pixel-override 0.01\nserver: asw.Off");
            _ = Task.Run(ToggleProximityBypass);
            _keepAwake.Start();
            Log("Rift maintenu éveillé (rafraîchi toutes les 9 min), rendu Rift réduit à 1 %.");
        }
        else StopKeepAwake();
    }

    private void StopKeepAwake()
    {
        _keepAwake.Stop();
        _ = RunOdtCli("service set-pixels-per-display-pixel-override 1");
        Log("Maintien éveillé du Rift arrêté.");
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string? cls, string? title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string? cls, string? title);
    [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("user32.dll")] private static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    private const uint WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CLOSE = 0x10;
    private const int VK_TAB = 0x09, VK_UP = 0x26, VK_DOWN = 0x28;

    private void ToggleProximityBypass()
    {
        var exe = OculusSupportDir() is { } d ? Path.Combine(d, "oculus-diagnostics", "OculusDebugTool.exe") : null;
        if (exe == null || !File.Exists(exe)) { Log("OculusDebugTool introuvable."); return; }
        IntPtr win;
        while ((win = FindWindow(null, "Oculus Debug Tool")) != IntPtr.Zero) { SendMessage(win, WM_CLOSE, IntPtr.Zero, IntPtr.Zero); Thread.Sleep(100); }
        Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
        for (int i = 0; i < 100 && (win = FindWindow(null, "Oculus Debug Tool")) == IntPtr.Zero; i++) Thread.Sleep(100);
        if (win == IntPtr.Zero) { Log("Oculus Debug Tool ne s'est pas ouvert."); return; }
        Thread.Sleep(800);
        var grid = FindWindowEx(FindWindowEx(win, IntPtr.Zero, "wxWindowNR", null), IntPtr.Zero, "wxWindow", null);
        var fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        AttachThreadInput(fg, GetCurrentThreadId(), true);
        SetForegroundWindow(win);
        AttachThreadInput(fg, GetCurrentThreadId(), false);
        Thread.Sleep(300);
        void Key(int k) { SendMessage(grid, WM_KEYDOWN, (IntPtr)k, IntPtr.Zero); SendMessage(grid, WM_KEYUP, (IntPtr)k, IntPtr.Zero); Thread.Sleep(60); }
        // "Bypass Proximity Sensor Check" is the 8th row; toggling it re-arms the 10 min timer.
        for (int i = 0; i < 7; i++) Key(VK_DOWN);
        Key(VK_TAB);
        Key(VK_UP);
        Thread.Sleep(100);
        Key(VK_DOWN);
        Thread.Sleep(150);
        SendMessage(win, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        Log($"Rift : capteur de proximité contourné ({DateTime.Now:HH:mm}).");
    }
}
