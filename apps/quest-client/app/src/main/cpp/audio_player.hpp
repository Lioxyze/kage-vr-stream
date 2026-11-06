// Low-latency playback of the PC audio stream (raw stereo int16 PCM) with AAudio.
// The network thread pushes; the AAudio callback pulls from a lock-free SPSC ring.
// A small jitter buffer (target ~30 ms) absorbs Wi-Fi jitter; anything above
// ~100 ms is dropped so latency cannot drift up over a session.
#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

class AudioPlayer {
public:
    ~AudioPlayer() { close(); }
    void push(uint32_t sampleRate, std::span<const int16_t> stereo);  // network thread
    void close();
    uint32_t underruns() const { return underruns_; }

private:
    bool open(uint32_t rate);
    static aaudio_data_callback_result_t dataCb(AAudioStream*, void* self, void* audio, int32_t frames);
    static void errorCb(AAudioStream*, void* self, aaudio_result_t);

    static constexpr uint64_t kCap = 1u << 15;  // frames (~680 ms at 48 kHz)
    std::unique_ptr<uint32_t[]> ring_{new uint32_t[kCap]};  // one stereo frame per slot
    std::atomic<uint64_t> w_{0}, r_{0};
    AAudioStream* stream_ = nullptr;
    uint32_t rate_ = 0, target_ = 0, high_ = 0;
    bool prefill_ = true;  // callback thread only
    std::atomic<bool> broken_{false};
    std::atomic<uint32_t> underruns_{0};
};
