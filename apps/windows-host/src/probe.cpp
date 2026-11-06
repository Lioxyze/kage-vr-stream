#include "probe.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <iphlpapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "iphlpapi.lib")

#ifndef MAKEFOURCC  // excluded by WIN32_LEAN_AND_MEAN (mmsystem.h)
#define MAKEFOURCC(a, b, c, d) \
    (static_cast<DWORD>(static_cast<BYTE>(a)) | (static_cast<DWORD>(static_cast<BYTE>(b)) << 8) | \
     (static_cast<DWORD>(static_cast<BYTE>(c)) << 16) | (static_cast<DWORD>(static_cast<BYTE>(d)) << 24))
#endif

using Microsoft::WRL::ComPtr;

namespace kage::host {
namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::optional<std::wstring> regString(HKEY root, const wchar_t* path, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return std::nullopt;
    return std::wstring(buf);
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t");
    const auto e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}

std::string probeOs() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const auto build = regString(HKEY_LOCAL_MACHINE, key, L"CurrentBuild");
    const auto display = regString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    const auto edition = regString(HKEY_LOCAL_MACHINE, key, L"EditionID");
    // ProductName still says "Windows 10" on Windows 11; the build number is authoritative.
    const int b = build ? _wtoi(build->c_str()) : 0;
    return std::format("Windows {} {} (build {}{})", b >= 22000 ? "11" : "10", edition ? utf8(*edition) : "",
                       build ? utf8(*build) : "?", display ? ", " + utf8(*display) : "");
}

std::string probeCpu() {
    auto v = regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    return v ? trim(utf8(*v)) : std::string{};
}

std::vector<GpuInfo> probeGpus() {
    std::vector<GpuInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    std::vector<LUID> seen;  // the same physical adapter can be enumerated more than once
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        const bool dup = std::any_of(seen.begin(), seen.end(), [&](const LUID& l) {
            return l.LowPart == d.AdapterLuid.LowPart && l.HighPart == d.AdapterLuid.HighPart;
        });
        if (dup) {
            adapter.Reset();
            continue;
        }
        seen.push_back(d.AdapterLuid);
        // Same physical GPU exposed through several DXGI adapters (e.g. with indirect/virtual displays).
        auto same = std::find_if(out.begin(), out.end(), [&](const GpuInfo& g) {
            return g.vendorId == d.VendorId && g.deviceId == d.DeviceId && g.subSysId == d.SubSysId &&
                   g.revision == d.Revision && g.name == utf8(d.Description);
        });
        if (same != out.end()) {
            ++same->dxgiEntries;
            adapter.Reset();
            continue;
        }
        GpuInfo g;
        g.name = utf8(d.Description);
        g.vendorId = d.VendorId;
        g.deviceId = d.DeviceId;
        g.subSysId = d.SubSysId;
        g.revision = d.Revision;
        g.dedicatedVideoMemory = d.DedicatedVideoMemory;
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            const auto v = static_cast<uint64_t>(umd.QuadPart);
            g.driverVersion = std::format("{}.{}.{}.{}", v >> 48, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF);
        }
        out.push_back(std::move(g));
        adapter.Reset();
    }
    return out;
}

// FourCC-based Media Foundation subtype, avoids depending on SDK-specific symbols (e.g. AV1).
GUID mfSubtype(DWORD fourcc) { return {fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}}; }

std::vector<std::string> hardwareEncoders(const GUID& subtype) {
    std::vector<std::string> names;
    MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, subtype};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                 nullptr, &outType, &acts, &count);
    if (FAILED(hr)) return names;
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* fn = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len))) {
            auto n = utf8(fn);
            if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(std::move(n));
            CoTaskMemFree(fn);
        }
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    return names;
}

std::vector<EncoderInfo> probeEncoders() {
    std::vector<EncoderInfo> out;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return out;
    out.push_back({"H.264", hardwareEncoders(mfSubtype(MAKEFOURCC('H', '2', '6', '4')))});
    out.push_back({"HEVC", hardwareEncoders(mfSubtype(MAKEFOURCC('H', 'E', 'V', 'C')))});
    out.push_back({"AV1", hardwareEncoders(mfSubtype(MAKEFOURCC('A', 'V', '0', '1')))});
    MFShutdown();
    return out;
}

AmfInfo probeAmf() {
    AmfInfo info;
    HMODULE m = LoadLibraryExW(L"amfrt64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return info;
    info.runtimePresent = true;
    // AMF_RESULT AMF_CDECL_CALL AMFQueryVersion(amf_uint64* pVersion) - see AMF SDK core/Factory.h
    using QueryVersionFn = int(__cdecl*)(uint64_t*);
    if (auto fn = reinterpret_cast<QueryVersionFn>(GetProcAddress(m, "AMFQueryVersion"))) {
        uint64_t v = 0;
        if (fn(&v) == 0 /* AMF_OK */)
            info.version = std::format("{}.{}.{}.{}", v >> 48, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF, v & 0xFFFF);
    }
    FreeLibrary(m);
    return info;
}

// Extracts the string array `key` from the (small, machine-written) openvrpaths.vrpath JSON.
std::vector<std::string> jsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    auto k = json.find("\"" + key + "\"");
    if (k == std::string::npos) return out;
    auto b = json.find('[', k);
    if (b == std::string::npos) return out;
    std::string cur;
    bool inStr = false;
    for (size_t i = b + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (!inStr) {
            if (c == ']') break;
            if (c == '"') {
                inStr = true;
                cur.clear();
            }
            continue;
        }
        if (c == '\\' && i + 1 < json.size()) {
            const char e = json[++i];
            cur += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
        } else if (c == '"') {
            inStr = false;
            out.push_back(cur);
        } else {
            cur += c;
        }
    }
    return out;
}

SteamVrInfo probeSteamVr() {
    SteamVrInfo info;
    wchar_t* local = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&local, &len, L"LOCALAPPDATA") != 0 || !local) return info;
    const std::filesystem::path p = std::filesystem::path(local) / L"openvr" / L"openvrpaths.vrpath";
    free(local);
    std::ifstream f(p, std::ios::binary);
    if (!f) return info;
    std::stringstream ss;
    ss << f.rdbuf();
    info.vrpathFound = true;
    info.runtimePaths = jsonStringArray(ss.str(), "runtime");
    info.externalDrivers = jsonStringArray(ss.str(), "external_drivers");
    for (const auto& r : info.runtimePaths) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(std::u8string(r.begin(), r.end())) / "bin" / "win64" / "vrserver.exe", ec))
            info.vrserverPresent = true;
    }
    return info;
}

std::vector<NetAdapterInfo> probeNetwork() {
    std::vector<NetAdapterInfo> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL)
            continue;
        NetAdapterInfo n;
        n.name = utf8(a->FriendlyName ? a->FriendlyName : L"");
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = a->Luid;
        const bool hardware = GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface;
        if (!hardware) n.type = "virtuel";
        else n.type = a->IfType == IF_TYPE_IEEE80211 ? "Wi-Fi" : a->IfType == IF_TYPE_ETHERNET_CSMACD ? "Ethernet" : "other";
        n.linkSpeedBps = a->TransmitLinkSpeed == ~0ull ? 0 : a->TransmitLinkSpeed;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[16];
            InetNtopA(AF_INET, &sin->sin_addr, ip, sizeof ip);
            n.ipv4.emplace_back(ip);
        }
        if (!n.ipv4.empty()) out.push_back(std::move(n));
    }
    return out;
}

const char* vendorName(uint32_t id) {
    switch (id) {
        case 0x1002: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        default: return "other";
    }
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void addWarnings(SystemReport& r) {
    const bool amdGpu = std::any_of(r.gpus.begin(), r.gpus.end(), [](auto& g) { return g.vendorId == 0x1002; });
    for (const auto& e : r.encoders) {
        if (e.hardwareMfts.empty())
            r.warnings.push_back(std::format("Aucun encodeur matériel {} détecté via Media Foundation.", e.codec));
    }
    if (amdGpu && !r.amf.runtimePresent)
        r.warnings.push_back("GPU AMD détecté mais runtime AMF (amfrt64.dll) introuvable : vérifier le pilote Adrenalin.");
    if (!r.steamvr.vrserverPresent)
        r.warnings.push_back("SteamVR introuvable (openvrpaths.vrpath / vrserver.exe) : nécessaire pour la phase 6.");
    for (const auto& d : r.steamvr.externalDrivers)
        r.warnings.push_back(std::format(
            "Autre pilote OpenVR enregistré : {} — à désactiver dans SteamVR pendant les sessions KAGÉ pour éviter un conflit de casque.",
            d));
    if (r.openxrActiveRuntime && lower(*r.openxrActiveRuntime).find("steamvr") == std::string::npos)
        r.warnings.push_back(std::format(
            "Runtime OpenXR actif : {} — les jeux OpenXR n'utiliseront SteamVR (donc KAGÉ) que si SteamVR est défini comme runtime OpenXR.",
            *r.openxrActiveRuntime));
    const bool eth = std::any_of(r.network.begin(), r.network.end(), [](auto& n) { return n.type == "Ethernet"; });
    const bool wifi = std::any_of(r.network.begin(), r.network.end(), [](auto& n) { return n.type == "Wi-Fi"; });
    if (wifi && !eth)
        r.warnings.push_back("Le PC n'est connecté qu'en Wi-Fi : une liaison Ethernet PC→routeur est recommandée pour le streaming.");
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) o += std::format("\\u{:04x}", c);
                else o += c;
        }
    }
    return o;
}

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string qarr(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + q(v[i]);
    return o + "]";
}

std::string orNa(const std::string& s) { return s.empty() ? "Non disponible" : s; }

}  // namespace

SystemReport probeSystem() {
    SystemReport r;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    r.os = probeOs();
    r.cpu = probeCpu();
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) r.ramBytes = ms.ullTotalPhys;
    r.gpus = probeGpus();
    r.encoders = probeEncoders();
    r.amf = probeAmf();
    r.steamvr = probeSteamVr();
    if (auto rt = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime"))
        r.openxrActiveRuntime = utf8(*rt);
    r.network = probeNetwork();
    addWarnings(r);
    if (SUCCEEDED(co)) CoUninitialize();
    return r;
}

std::string toText(const SystemReport& r) {
    std::string o;
    o += std::format("Système      : {}\n", orNa(r.os));
    o += std::format("CPU          : {}\n", orNa(r.cpu));
    o += std::format("RAM          : {}\n", r.ramBytes ? std::format("{:.1f} Gio", r.ramBytes / 1073741824.0) : "Non disponible");
    o += "GPU (DXGI)   :\n";
    for (const auto& g : r.gpus)
        o += std::format("  - {} [{} {:04X}:{:04X}] VRAM {} Mio, pilote {}{}{}\n", g.name, vendorName(g.vendorId),
                         g.vendorId, g.deviceId, g.dedicatedVideoMemory / (1024 * 1024), orNa(g.driverVersion),
                         g.software ? " (logiciel)" : "",
                         g.dxgiEntries > 1 ? std::format(" ({} entrées DXGI)", g.dxgiEntries) : "");
    o += "Encodeurs matériels (Media Foundation) :\n";
    for (const auto& e : r.encoders) {
        o += std::format("  - {:6}: ", e.codec);
        if (e.hardwareMfts.empty()) o += "aucun\n";
        for (size_t i = 0; i < e.hardwareMfts.size(); ++i) o += (i ? ", " : "") + e.hardwareMfts[i] + (i + 1 == e.hardwareMfts.size() ? "\n" : "");
    }
    o += std::format("Runtime AMF  : {}\n", r.amf.runtimePresent ? "présent, version " + r.amf.version.value_or("Non disponible") : "absent");
    o += std::format("SteamVR      : {}\n", r.steamvr.vrserverPresent ? "installé" : "introuvable");
    for (const auto& p : r.steamvr.runtimePaths) o += std::format("  runtime    : {}\n", p);
    for (const auto& d : r.steamvr.externalDrivers) o += std::format("  pilote ext.: {}\n", d);
    o += std::format("OpenXR actif : {}\n", r.openxrActiveRuntime.value_or("Non disponible"));
    o += "Réseau       :\n";
    for (const auto& n : r.network) {
        std::string ips;
        for (size_t i = 0; i < n.ipv4.size(); ++i) ips += (i ? ", " : "") + n.ipv4[i];
        o += std::format("  - {} ({}), {} — {}\n", n.name, n.type,
                         n.linkSpeedBps ? std::format("{} Mbit/s", n.linkSpeedBps / 1000000) : "débit Non disponible", ips);
    }
    if (!r.warnings.empty()) {
        o += "\nObservations :\n";
        for (const auto& w : r.warnings) o += "  ! " + w + "\n";
    }
    return o;
}

std::string toJson(const SystemReport& r) {
    std::string o = "{\n";
    o += std::format("  \"os\": {},\n  \"cpu\": {},\n  \"ramBytes\": {},\n", q(r.os), q(r.cpu), r.ramBytes);
    o += "  \"gpus\": [";
    for (size_t i = 0; i < r.gpus.size(); ++i) {
        const auto& g = r.gpus[i];
        o += std::format("{}\n    {{\"name\": {}, \"vendorId\": {}, \"deviceId\": {}, \"dedicatedVideoMemory\": {}, \"driverVersion\": {}, \"software\": {}, \"dxgiEntries\": {}}}",
                         i ? "," : "", q(g.name), g.vendorId, g.deviceId, g.dedicatedVideoMemory,
                         g.driverVersion.empty() ? "null" : q(g.driverVersion), g.software ? "true" : "false", g.dxgiEntries);
    }
    o += "\n  ],\n  \"hardwareEncoders\": {";
    for (size_t i = 0; i < r.encoders.size(); ++i)
        o += std::format("{}\n    {}: {}", i ? "," : "", q(r.encoders[i].codec), qarr(r.encoders[i].hardwareMfts));
    o += "\n  },\n";
    o += std::format("  \"amf\": {{\"runtimePresent\": {}, \"version\": {}}},\n", r.amf.runtimePresent ? "true" : "false",
                     r.amf.version ? q(*r.amf.version) : "null");
    o += std::format("  \"steamvr\": {{\"vrserverPresent\": {}, \"runtimePaths\": {}, \"externalDrivers\": {}}},\n",
                     r.steamvr.vrserverPresent ? "true" : "false", qarr(r.steamvr.runtimePaths), qarr(r.steamvr.externalDrivers));
    o += std::format("  \"openxrActiveRuntime\": {},\n", r.openxrActiveRuntime ? q(*r.openxrActiveRuntime) : "null");
    o += "  \"network\": [";
    for (size_t i = 0; i < r.network.size(); ++i) {
        const auto& n = r.network[i];
        o += std::format("{}\n    {{\"name\": {}, \"type\": {}, \"linkSpeedBps\": {}, \"ipv4\": {}}}", i ? "," : "", q(n.name),
                         q(n.type), n.linkSpeedBps, qarr(n.ipv4));
    }
    o += "\n  ],\n  \"warnings\": " + qarr(r.warnings) + "\n}\n";
    return o;
}

}  // namespace kage::host
