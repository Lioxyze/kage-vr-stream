// Hardware video decoding (AMediaCodec, async, low-latency flags) into an
// AImageReader; the newest decoded image is bound as an external OES texture
// through EGLImage — no CPU copy of pixels anywhere.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

class VideoDecoder {
public:
    ~VideoDecoder();
    bool init(bool hevc, int width, int height);
    void shutdown();
    bool ready() const { return codec_ != nullptr; }

    // Network thread: queue one complete access unit (frame index used as PTS).
    void submit(uint32_t frameIndex, const uint8_t* data, size_t size, uint64_t nowUs);

    // Render thread: binds the newest decoded frame (if any new one) to texture().
    // Returns true when a new frame was latched; frameIndex() identifies it.
    bool latest(EGLDisplay display);
    GLuint texture() const { return tex_; }
    uint32_t frameIndex() const { return frameIndex_; }
    bool hasFrame() const { return hasFrame_; }

    // Average decode latency (queue -> image available) since last call, µs.
    uint32_t takeDecodeUsAvg();

private:
    static void onInput(AMediaCodec*, void* self, int32_t index);
    static void onOutput(AMediaCodec*, void* self, int32_t index, AMediaCodecBufferInfo* info);
    static void onFormat(AMediaCodec*, void*, AMediaFormat*) {}
    static void onError(AMediaCodec*, void*, media_status_t err, int32_t, const char* detail);
    void pump();

    AMediaCodec* codec_ = nullptr;
    AImageReader* reader_ = nullptr;
    std::mutex mutex_;
    std::deque<int32_t> freeInputs_;
    struct Pending {
        uint32_t index;
        std::vector<uint8_t> data;
    };
    std::deque<Pending> pending_;
    uint64_t queuedAt_[64] = {};

    GLuint tex_ = 0;
    AImage* current_ = nullptr;
    AImage* previous_ = nullptr;
    std::map<AHardwareBuffer*, EGLImageKHR> images_;
    uint32_t frameIndex_ = 0;
    bool hasFrame_ = false;
    std::atomic<uint64_t> decodeUsSum_{0}, decodeCount_{0};
};
