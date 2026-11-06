#include "audio_player.hpp"

#include <algorithm>
#include <cstring>

#include "kage/util/log.hpp"

namespace klog = kage::log;

bool AudioPlayer::open(uint32_t rate) {
    close();
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setSampleRate(b, static_cast<int32_t>(rate));
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setDataCallback(b, &AudioPlayer::dataCb, this);
    AAudioStreamBuilder_setErrorCallback(b, &AudioPlayer::errorCb, this);
    const aaudio_result_t res = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (res != AAUDIO_OK) {
        klog::error("audio: AAudio open failed ({})", AAudio_convertResultToText(res));
        stream_ = nullptr;
        return false;
    }
    // Smallest buffer the device accepts: two bursts.
    AAudioStream_setBufferSizeInFrames(stream_, AAudioStream_getFramesPerBurst(stream_) * 2);
    rate_ = rate;
    target_ = rate * 30 / 1000;
    high_ = rate * 100 / 1000;
    prefill_ = true;
    r_ = w_.load();
    broken_ = false;
    AAudioStream_requestStart(stream_);
    klog::info("audio: playing {} Hz, burst {} frames, device rate {}", rate, AAudioStream_getFramesPerBurst(stream_),
               AAudioStream_getSampleRate(stream_));
    return true;
}

void AudioPlayer::close() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    rate_ = 0;
}

void AudioPlayer::push(uint32_t sampleRate, std::span<const int16_t> stereo) {
    if (sampleRate < 8000 || sampleRate > 192000) return;
    if ((!stream_ || broken_ || sampleRate != rate_) && !open(sampleRate)) return;
    const size_t frames = stereo.size() / 2;
    const uint64_t w = w_.load(std::memory_order_relaxed);
    if (w + frames - r_.load(std::memory_order_acquire) > kCap) return;  // full: drop
    for (size_t i = 0; i < frames; ++i) {
        uint32_t v;
        std::memcpy(&v, stereo.data() + i * 2, 4);
        ring_[(w + i) & (kCap - 1)] = v;
    }
    w_.store(w + frames, std::memory_order_release);
}

aaudio_data_callback_result_t AudioPlayer::dataCb(AAudioStream*, void* self, void* audio, int32_t frames) {
    auto* p = static_cast<AudioPlayer*>(self);
    auto* out = static_cast<uint32_t*>(audio);
    const uint64_t w = p->w_.load(std::memory_order_acquire);
    uint64_t r = p->r_.load(std::memory_order_relaxed);
    uint64_t avail = w - r;
    if (p->prefill_) {
        if (avail < p->target_) {
            std::memset(out, 0, static_cast<size_t>(frames) * 4);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        p->prefill_ = false;
    }
    if (avail > p->high_) {  // latency crept up (burst after a stall): skip back to the target
        r = w - p->target_;
        avail = p->target_;
    }
    const uint64_t n = std::min<uint64_t>(avail, static_cast<uint64_t>(frames));
    for (uint64_t i = 0; i < n; ++i) out[i] = p->ring_[(r + i) & (kCap - 1)];
    if (n < static_cast<uint64_t>(frames)) {
        std::memset(out + n, 0, static_cast<size_t>(frames - n) * 4);
        p->prefill_ = true;  // PC is silent or the network stalled: rebuild the cushion
        p->underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    p->r_.store(r + n, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioPlayer::errorCb(AAudioStream*, void* self, aaudio_result_t err) {
    // Device change/disconnect: reopened from the network thread on the next packet.
    klog::warn("audio: stream error {}", AAudio_convertResultToText(err));
    static_cast<AudioPlayer*>(self)->broken_ = true;
}
