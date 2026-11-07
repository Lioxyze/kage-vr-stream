// Plays the headset microphone into a Windows playback device whose recording side
// is then used as the mic (Voicemeeter AUX/VAIO3 input, VB-CABLE...). Device picked by
// name: %LOCALAPPDATA%\KageVR\mic_device.txt (part of the name), else the first known
// virtual input found. Nothing is installed; nothing plays if none exists.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace kage::driver {
class MicPlayer {
public:
    void start(const std::filesystem::path& dataDir);
    void stop();
    void push(uint32_t rate, std::span<const int16_t> mono);

private:
    void loop();
    bool playOnce();
    std::filesystem::path dataDir_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex m_;
    std::vector<int16_t> ring_;  // pending samples (mono, 48 kHz)
    uint64_t lastPushMs_ = 0;
};
}  // namespace kage::driver
