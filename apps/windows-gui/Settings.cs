// "Réglages" tab: PC streaming settings read by the SteamVR driver
// (%LOCALAPPDATA%\KageVR\settings.ini). Applied at the next SteamVR start.
using System;
using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace KageVR.Gui;

public partial class MainWindow
{
    static readonly string SettingsFile = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KageVR", "settings.ini");
    static readonly string[] EncoderKeys = { "auto", "amf", "gpu", "software" };
    static readonly string[] CodecKeys = { "auto", "h264" };

    void LoadSettingsUi()
    {
        string enc = "auto", codec = "auto";
        int mbit = 0;
        try
        {
            foreach (var line in File.ReadAllLines(SettingsFile))
            {
                var kv = line.Split('=', 2);
                if (kv.Length != 2) continue;
                var v = kv[1].Trim().ToLowerInvariant();
                if (kv[0] == "encoder") enc = v;
                else if (kv[0] == "codec") codec = v;
                else if (kv[0] == "maxbitrate") int.TryParse(v, out mbit);
            }
        }
        catch { }
        EncoderBox.SelectedIndex = Math.Max(0, Array.IndexOf(EncoderKeys, enc));
        CodecBox.SelectedIndex = Math.Max(0, Array.IndexOf(CodecKeys, codec));
        BitrateSlider.Value = mbit;
        UpdateBitrateLabel();
    }

    void UpdateBitrateLabel()
    {
        if (BitrateLabel == null) return;
        int v = (int)BitrateSlider.Value;
        BitrateLabel.Text = v == 0 ? "Débit max : choix du casque (qualité Basse/Moyenne/Haute/Max)" : $"Débit max : {v} Mbit/s";
    }

    void OnBitrateChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => UpdateBitrateLabel();

    void OnSaveSettings(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsFile)!);
            File.WriteAllText(SettingsFile,
                $"encoder={EncoderKeys[Math.Max(0, EncoderBox.SelectedIndex)]}\n" +
                $"codec={CodecKeys[Math.Max(0, CodecBox.SelectedIndex)]}\n" +
                $"maxbitrate={(int)BitrateSlider.Value}\n");
            SettingsStatus.Text = "Enregistré. Pris en compte au prochain lancement de SteamVR (ferme SteamVR puis reconnecte le casque).";
        }
        catch (Exception ex)
        {
            SettingsStatus.Text = "Erreur : " + ex.Message;
        }
    }
}
