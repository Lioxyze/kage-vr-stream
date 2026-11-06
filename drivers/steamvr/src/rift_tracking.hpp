// Full body tracking with an Oculus Rift CV1 (replaces OculusTouchLink +
// OpenVR-SpaceCalibrator + external smoothing):
//  - reads the Rift Touch controllers and "VR objects" (3rd Touch) through LibOVR
//    (invisible session: the Rift renders nothing for us);
//  - exposes them to SteamVR as trackers (left foot, right foot, waist);
//  - built-in calibration: hold the Rift right Touch against the Quest right
//    controller and move/rotate for ~15 s; solved for yaw + translation (both
//    spaces are gravity aligned), saved and reused across sessions;
//  - One Euro smoothing (low jitter at rest, low lag when moving).
// Enabled by the file %LOCALAPPDATA%\KageVR\fullbody.enabled (GUI checkbox).
#pragma once

#include <filesystem>
#include <memory>

#include "kage/protocol/messages.hpp"

namespace kage::driver {

class RiftTracking {
public:
    RiftTracking();
    ~RiftTracking();
    // Loads LibOVR from the Meta runtime and registers the trackers. False if no Rift.
    bool start(const std::filesystem::path& dataDir);
    void stop();
    // Quest right controller grip pose (stage space), from the network thread.
    void setReference(const protocol::ControllerState& questRight);
    bool started() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kage::driver
