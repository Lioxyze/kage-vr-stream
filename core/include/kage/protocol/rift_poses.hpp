// Local link (127.0.0.1 only) kage-host -> SteamVR driver carrying the Rift CV1
// poses for full body tracking. The Rift is read through LibOVR in kage-host, OUT
// of SteamVR: inside vrserver it would clash with SteamVR's own Oculus driver, which
// Virtual Desktop relies on (same design as OculusTouchLink).
// Reader -> driver: magic, 3 x {flags, position xyz, rotation wxyz} in Rift space.
// Driver -> reader: magic, f32 prediction horizon (s) to sample the Rift at.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace kage::protocol {

inline constexpr uint16_t kRiftPosePort = 47803;
inline constexpr uint32_t kRiftMagic = 0x5446524Bu;  // "KRFT"

enum RiftPoseFlags : uint8_t { kRiftOrientation = 1, kRiftPosition = 2 };

struct RiftDevicePose {
    uint8_t flags = 0;
    float pos[3] = {};
    float rot[4] = {1, 0, 0, 0};  // w x y z
};
using RiftPoses = std::array<RiftDevicePose, 3>;  // left Touch, right Touch, VR object 0

inline void encodeRiftPoses(const RiftPoses& p, std::vector<uint8_t>& out) {
    out.resize(4 + 3 * 29);
    std::memcpy(out.data(), &kRiftMagic, 4);  // both ends are little-endian x64 Windows
    uint8_t* w = out.data() + 4;
    for (const auto& d : p) {
        *w++ = d.flags;
        std::memcpy(w, d.pos, 12);
        std::memcpy(w + 12, d.rot, 16);
        w += 28;
    }
}

inline bool decodeRiftPoses(std::span<const uint8_t> in, RiftPoses& p) {
    uint32_t magic = 0;
    if (in.size() != 4 + 3 * 29) return false;
    std::memcpy(&magic, in.data(), 4);
    if (magic != kRiftMagic) return false;
    const uint8_t* r = in.data() + 4;
    for (auto& d : p) {
        d.flags = *r++;
        std::memcpy(d.pos, r, 12);
        std::memcpy(d.rot, r + 12, 16);
        r += 28;
        for (float v : d.pos)
            if (!std::isfinite(v) || std::fabs(v) > 100.f) return false;
        float n = 0;
        for (float v : d.rot) n += v * v;
        if (d.flags && (!std::isfinite(n) || std::fabs(n - 1.f) > 0.05f)) return false;
    }
    return true;
}

}  // namespace kage::protocol
