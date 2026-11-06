#include "video_decoder.hpp"

#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>

#include <cstring>

#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace klog = kage::log;

namespace {
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNativeClientBuffer = nullptr;
PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;
}  // namespace

VideoDecoder::~VideoDecoder() { shutdown(); }

bool VideoDecoder::init(bool hevc, int width, int height) {
    pGetNativeClientBuffer =
        reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!pGetNativeClientBuffer || !pCreateImage || !pImageTargetTexture) {
        klog::error("decoder: EGLImage extensions missing");
        return false;
    }
    if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4,
                                  &reader_) != AMEDIA_OK)
        return false;
    ANativeWindow* window = nullptr;
    AImageReader_getWindow(reader_, &window);

    const char* mime = hevc ? "video/hevc" : "video/avc";
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) {
        klog::error("decoder: no {} decoder", mime);
        return false;
    }
    AMediaFormat* f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height);
    // Latency-first decoder configuration (standard + Qualcomm vendor keys).
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "priority", 0);  // real-time
    AMediaFormat_setInt32(f, "operating-rate", 32767);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaCodecOnAsyncNotifyCallback cb{onInput, onOutput, onFormat, onError};
    AMediaCodec_setAsyncNotifyCallback(codec_, cb, this);
    const media_status_t st = AMediaCodec_configure(codec_, f, window, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
        klog::error("decoder: configure/start failed ({})", static_cast<int>(st));
        return false;
    }
    char* name = nullptr;
    if (AMediaCodec_getName(codec_, &name) == AMEDIA_OK) {
        klog::info("decoder: {} {}x{}", name, width, height);
        AMediaCodec_releaseName(codec_, name);
    }
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void VideoDecoder::shutdown() {
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    EGLDisplay d = eglGetCurrentDisplay();
    for (auto& [buf, img] : images_)
        if (pDestroyImage && d != EGL_NO_DISPLAY) pDestroyImage(d, img);
    images_.clear();
    if (previous_) AImage_delete(previous_);
    if (current_) AImage_delete(current_);
    previous_ = current_ = nullptr;
    if (reader_) {
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
    if (tex_) glDeleteTextures(1, &tex_);
    tex_ = 0;
    hasFrame_ = false;
    std::lock_guard l(mutex_);
    freeInputs_.clear();
    pending_.clear();
}

void VideoDecoder::onInput(AMediaCodec*, void* self, int32_t index) {
    auto* d = static_cast<VideoDecoder*>(self);
    std::lock_guard l(d->mutex_);
    d->freeInputs_.push_back(index);
    d->pump();
}

void VideoDecoder::onOutput(AMediaCodec* codec, void* self, int32_t index, AMediaCodecBufferInfo* info) {
    auto* d = static_cast<VideoDecoder*>(self);
    const uint32_t frame = static_cast<uint32_t>(info->presentationTimeUs);
    const uint64_t q = d->queuedAt_[frame % 64];
    if (q) {
        d->decodeUsSum_ += kage::monotonicMicros() - q;
        ++d->decodeCount_;
    }
    AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), true);  // render into the ImageReader
}

void VideoDecoder::onError(AMediaCodec*, void*, media_status_t err, int32_t, const char* detail) {
    klog::error("decoder error {}: {}", static_cast<int>(err), detail ? detail : "");
}

void VideoDecoder::pump() {  // mutex held
    while (!freeInputs_.empty() && !pending_.empty()) {
        const int32_t idx = freeInputs_.front();
        auto p = std::move(pending_.front());
        pending_.pop_front();
        freeInputs_.pop_front();
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
        if (!buf || cap < p.data.size()) {
            klog::warn("decoder input buffer too small ({} < {})", cap, p.data.size());
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0, 0);
            continue;
        }
        std::memcpy(buf, p.data.data(), p.data.size());
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, p.data.size(), p.index, 0);
    }
}

void VideoDecoder::submit(uint32_t frameIndex, const uint8_t* data, size_t size, uint64_t nowUs) {
    if (!codec_) return;
    std::lock_guard l(mutex_);
    queuedAt_[frameIndex % 64] = nowUs;
    pending_.push_back({frameIndex, std::vector<uint8_t>(data, data + size)});
    pump();
}

bool VideoDecoder::latest(EGLDisplay display) {
    if (!reader_) return false;
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader_, &img) != AMEDIA_OK || !img) return false;
    AHardwareBuffer* hb = nullptr;
    AImage_getHardwareBuffer(img, &hb);
    if (!hb) {
        AImage_delete(img);
        return false;
    }
    EGLImageKHR eimg;
    auto it = images_.find(hb);
    if (it == images_.end()) {
        const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        eimg = pCreateImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, pGetNativeClientBuffer(hb), attrs);
        images_[hb] = eimg;
    } else {
        eimg = it->second;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(eimg));
    int64_t ts = 0;
    AImage_getTimestamp(img, &ts);
    frameIndex_ = static_cast<uint32_t>(ts / 1000);
    // Keep the image currently sampled and the previous one (GPU may still read it) alive.
    if (previous_) AImage_delete(previous_);
    previous_ = current_;
    current_ = img;
    hasFrame_ = true;
    return true;
}

uint32_t VideoDecoder::takeDecodeUsAvg() {
    const uint64_t n = decodeCount_.exchange(0), s = decodeUsSum_.exchange(0);
    return n ? static_cast<uint32_t>(s / n) : 0;
}
