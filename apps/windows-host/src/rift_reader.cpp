// Reads the Rift CV1 Touch controllers + VR object through LibOVR (invisible session)
// and streams their poses to the KAGE SteamVR driver on 127.0.0.1 (see rift_poses.hpp).
// Runs inside kage-host, outside SteamVR, so it never conflicts with SteamVR's Oculus
// driver (needed by Virtual Desktop).
#include "rift_reader.hpp"

#include <windows.h>

#include <OVR_CAPI.h>

#include <array>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "kage/net/udp_socket.hpp"
#include "kage/protocol/rift_poses.hpp"
#include "kage/util/log.hpp"

namespace kage::host {

namespace kp = kage::protocol;

namespace {
void status(const std::filesystem::path& dir, const std::string& s) {
    std::ofstream(dir / L"fullbody_status.txt", std::ios::trunc) << s;
    kage::log::info("rift: {}", s);
}
}  // namespace

void runRiftReader(const std::atomic<bool>& stop, const std::filesystem::path& dataDir) {
    auto sock = net::UdpSocket::open(0, 0x7F000001u, false);
    if (!sock) return;
    const auto driver = net::Endpoint::loopback(kp::kRiftPosePort);
    std::string lastErr;
    std::vector<uint8_t> out;
    std::array<uint8_t, 64> in{};
    while (!stop) {
        // (Re)connect: Meta Horizon Link may start or the Rift be plugged later.
        ovrInitParams p{ovrInit_RequestVersion | ovrInit_Invisible | ovrInit_FocusAware, OVR_MINOR_VERSION, nullptr, 0, 0};
        ovrSession session = nullptr;
        ovrGraphicsLuid luid;
        std::string err;
        if (OVR_FAILURE(ovr_Initialize(&p))) err = "Rift : runtime Meta indisponible (Meta Horizon Link installé et lancé ?)";
        else if (OVR_FAILURE(ovr_Create(&session, &luid))) {
            ovr_Shutdown();
            err = "Rift : casque CV1 non détecté (branché, Meta Horizon Link lancé ?)";
        }
        if (!err.empty()) {
            if (err != lastErr) status(dataDir, err);
            lastErr = err;
            for (int i = 0; i < 50 && !stop; ++i) Sleep(100);
            continue;
        }
        lastErr.clear();
        ovr_SetTrackingOriginType(session, ovrTrackingOrigin_FloorLevel);
        status(dataDir, "Rift connecté : lancer SteamVR (KAGÉ ou Virtual Desktop)");
        double predictS = 0.03;  // updated by the driver
        while (!stop) {
            ovrSessionStatus ss{};
            if (OVR_FAILURE(ovr_GetSessionStatus(session, &ss)) || ss.ShouldQuit || !ss.HmdPresent) break;
            const double at = ovr_GetTimeInSeconds() + predictS;
            const ovrTrackingState ts = ovr_GetTrackingState(session, at, ovrFalse);
            ovrTrackedDeviceType objType = ovrTrackedDevice_Object0;
            ovrPoseStatef obj{};
            const bool objOk = (ovr_GetConnectedControllerTypes(session) & ovrControllerType_Object0) &&
                               OVR_SUCCESS(ovr_GetDevicePoses(session, &objType, 1, at, &obj));
            kp::RiftPoses poses;
            const ovrPoseStatef* src[3] = {&ts.HandPoses[0], &ts.HandPoses[1], &obj};
            const unsigned flags[3] = {ts.HandStatusFlags[0], ts.HandStatusFlags[1],
                                       objOk ? static_cast<unsigned>(ovrStatus_PositionTracked | ovrStatus_OrientationTracked) : 0u};
            for (int i = 0; i < 3; ++i) {
                auto& d = poses[i];
                d.flags = static_cast<uint8_t>(((flags[i] & ovrStatus_OrientationTracked) ? kp::kRiftOrientation : 0) |
                                               ((flags[i] & ovrStatus_PositionTracked) ? kp::kRiftPosition : 0));
                const auto& pp = src[i]->ThePose;
                d.pos[0] = pp.Position.x;
                d.pos[1] = pp.Position.y;
                d.pos[2] = pp.Position.z;
                d.rot[0] = pp.Orientation.w;
                d.rot[1] = pp.Orientation.x;
                d.rot[2] = pp.Orientation.y;
                d.rot[3] = pp.Orientation.z;
                if (!d.flags) d.rot[0] = 1, d.rot[1] = d.rot[2] = d.rot[3] = 0;
            }
            kp::encodeRiftPoses(poses, out);
            sock->sendTo(driver, out);
            // Prediction horizon from the driver (non-blocking).
            size_t n = 0;
            net::Endpoint from;
            while (sock->receive(in, n, from, 0) == net::RecvStatus::Ok)
                if (n == 8 && from.ipv4 == 0x7F000001u) {
                    uint32_t magic;
                    float pr;
                    std::memcpy(&magic, in.data(), 4);
                    std::memcpy(&pr, in.data() + 4, 4);
                    if (magic == kp::kRiftMagic && pr >= 0 && pr < 0.2f) predictS = pr;
                }
            Sleep(3);  // ~300 Hz
        }
        ovr_Destroy(session);
        ovr_Shutdown();
        status(dataDir, "Rift déconnecté");
    }
}

}  // namespace kage::host
