// PC-side streaming settings written by the KageVR app (Réglages tab):
// %LOCALAPPDATA%\KageVR\settings.ini, one "key=value" per line. No limits are enforced:
// whatever the user picks is tried.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace kage::driver {

struct Settings {
    std::string encoder = "auto";  // auto | amf | gpu | software
    std::string codec = "auto";    // auto (HEVC) | hevc | h264
    uint32_t maxBitrateKbps = 0;   // 0 = headset choice
    // After a lost frame: "refresh" = the encoder heals the picture progressively (no keyframe
    // spike, no freeze; brief smearing possible), "idr" = wait for a keyframe (old behaviour).
    std::string recovery = "refresh";
    bool slices = true;  // send each slice as soon as it is encoded
};

inline Settings loadSettings() {
    Settings s;
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return s;
    std::ifstream f(std::filesystem::path(local) / L"KageVR" / L"settings.ini");
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "encoder") s.encoder = v;
        else if (k == "codec") s.codec = v;
        else if (k == "recovery") s.recovery = v;
        else if (k == "slices") s.slices = v != "0" && v != "off" && v != "false";
        else if (k == "maxbitrate") s.maxBitrateKbps = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10)) * 1000;
    }
    return s;
}

}  // namespace kage::driver
