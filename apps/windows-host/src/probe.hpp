// KAGE VR Stream host - real hardware / software environment detection.
// Every field is read from the system; anything that cannot be measured is
// left empty / nullopt and reported as "Non disponible", never guessed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kage::host {

struct GpuInfo {
    std::string name;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t subSysId = 0;
    uint32_t revision = 0;
    uint32_t dxgiEntries = 1;  // number of DXGI adapters exposing this same GPU
    uint64_t dedicatedVideoMemory = 0;
    std::string driverVersion;  // UMD version from DXGI, e.g. 32.0.21045.16010
    bool software = false;
};

struct EncoderInfo {
    std::string codec;  // "H.264", "HEVC", "AV1"
    std::vector<std::string> hardwareMfts;  // Media Foundation hardware encoders found
};

struct AmfInfo {
    bool runtimePresent = false;  // amfrt64.dll loadable
    std::optional<std::string> version;
};

struct SteamVrInfo {
    bool vrpathFound = false;
    std::vector<std::string> runtimePaths;
    bool vrserverPresent = false;
    std::vector<std::string> externalDrivers;  // other OpenVR drivers registered (VD, ALVR, ...)
};

struct NetAdapterInfo {
    std::string name;
    std::string type;  // "Ethernet", "Wi-Fi", "other"
    uint64_t linkSpeedBps = 0;
    std::vector<std::string> ipv4;
};

struct SystemReport {
    std::string os;
    std::string cpu;
    uint64_t ramBytes = 0;
    std::vector<GpuInfo> gpus;
    std::vector<EncoderInfo> encoders;
    AmfInfo amf;
    SteamVrInfo steamvr;
    std::optional<std::string> openxrActiveRuntime;
    std::vector<NetAdapterInfo> network;
    std::vector<std::string> warnings;  // human-readable, evidence-based
};

SystemReport probeSystem();
std::string toText(const SystemReport& r);
std::string toJson(const SystemReport& r);

}  // namespace kage::host
