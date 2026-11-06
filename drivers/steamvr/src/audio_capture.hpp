// System audio capture (WASAPI loopback of the default output device), converted
// to 16-bit stereo PCM and handed out in small chunks (<= 5 ms) for the network.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>

namespace kage::driver {

class AudioCapture {
public:
    // Called on the capture thread: sampleRate, interleaved stereo int16 samples.
    std::function<void(uint32_t, std::span<const int16_t>)> onChunk;
    std::function<bool()> enabled;  // capture only while a headset is connected

    void start();
    void stop();
    ~AudioCapture() { stop(); }

private:
    void loop();
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace kage::driver
