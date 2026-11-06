#include "amf_encoder.hpp"

#include <windows.h>

#include <algorithm>

#include "components/VideoEncoderHEVC.h"
#include "components/VideoEncoderVCE.h"
#include "core/Factory.h"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace kage::driver {
namespace {
constexpr const wchar_t* kPropFrameIndex = L"KageFrameIndex";
constexpr const wchar_t* kPropSubmitUs = L"KageSubmitUs";

#define AMF_TRY(expr)                                                       \
    do {                                                                    \
        AMF_RESULT r_ = (expr);                                             \
        if (r_ != AMF_OK) {                                                 \
            log::error("AMF {} failed: {}", #expr, static_cast<int>(r_));   \
            return false;                                                   \
        }                                                                   \
    } while (0)

// Sets a property, logging (not failing) when the runtime does not know it.
template <class T>
void setOpt(amf::AMFComponent* c, const wchar_t* name, T v) {
    if (c->SetProperty(name, v) != AMF_OK) {
        char n[128];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof n, nullptr, nullptr);
        log::warn("AMF: property {} not supported, ignored", n);
    }
}
}  // namespace

AmfEncoder::~AmfEncoder() { shutdown(); }

bool AmfEncoder::init(ID3D11Device* device, bool hevc, uint32_t width, uint32_t height, float fps, uint32_t kbps) {
    hevc_ = hevc;
    fps_ = fps;
    HMODULE dll = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        log::error("AMF runtime {} not found", "amfrt64.dll");
        return false;
    }
    dll_ = dll;
    auto queryVersion = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(dll, AMF_QUERY_VERSION_FUNCTION_NAME));
    auto initFn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll, AMF_INIT_FUNCTION_NAME));
    if (!queryVersion || !initFn) return false;
    amf_uint64 runtimeVersion = 0;
    AMF_TRY(queryVersion(&runtimeVersion));
    // Ask for the older of (headers, runtime) so a newer SDK header never blocks an older driver.
    AMF_TRY(initFn(std::min<amf_uint64>(runtimeVersion, AMF_FULL_VERSION), &factory_));
    AMF_TRY(factory_->CreateContext(&context_));
    AMF_TRY(context_->InitDX11(device));
    AMF_TRY(factory_->CreateComponent(context_, hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &encoder_));

    const amf_int64 bps = static_cast<amf_int64>(kbps) * 1000;
    const AMFRate rate = AMFConstructRate(static_cast<amf_uint32>(fps + 0.5f), 1);
    // One frame of VBV: bounds the size of any single frame => bounds network burst/latency.
    const amf_int64 vbv = static_cast<amf_int64>(bps / fps);
    if (hevc) {
        AMF_TRY(encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY));
        // At <= 72 fps the frame budget allows the better BALANCED preset (sharper image).
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
               fps <= 72.5f ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED);
        AMF_TRY(encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, AMFConstructSize(width, height)));
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, true);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_MAX_NUM_REFRAMES, static_cast<amf_int64>(1));
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, static_cast<amf_int64>(fps * 10));  // safety IDR every 10 s
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
               AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
        setOpt(encoder_, AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, static_cast<amf_int64>(10));
    } else {
        AMF_TRY(encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY));
        setOpt(encoder_, AMF_VIDEO_ENCODER_QUALITY_PRESET,
               fps <= 72.5f ? AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED : AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
        AMF_TRY(encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, AMFConstructSize(width, height)));
        setOpt(encoder_, AMF_VIDEO_ENCODER_FRAMERATE, rate);
        setOpt(encoder_, AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        setOpt(encoder_, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
        setOpt(encoder_, AMF_VIDEO_ENCODER_TARGET_BITRATE, bps);
        setOpt(encoder_, AMF_VIDEO_ENCODER_PEAK_BITRATE, bps);
        setOpt(encoder_, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv);
        setOpt(encoder_, AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        setOpt(encoder_, AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
        setOpt(encoder_, AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(fps * 10));
        setOpt(encoder_, AMF_VIDEO_ENCODER_QUERY_TIMEOUT, static_cast<amf_int64>(10));
    }
    AMF_TRY(encoder_->Init(amf::AMF_SURFACE_BGRA, static_cast<amf_int32>(width), static_cast<amf_int32>(height)));
    log::info("AMF {} encoder ready: {}x{} @ {:.0f} Hz, {} Mbit/s CBR, ultra-low-latency, runtime {}.{}.{}",
              hevc ? "HEVC" : "H.264", width, height, fps, kbps / 1000, runtimeVersion >> 48,
              (runtimeVersion >> 32) & 0xFFFF, (runtimeVersion >> 16) & 0xFFFF);
    return true;
}

void AmfEncoder::shutdown() {
    if (encoder_) {
        encoder_->Terminate();
        encoder_->Release();
        encoder_ = nullptr;
    }
    if (context_) {
        context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }
    factory_ = nullptr;  // owned by the runtime
    if (dll_) {
        FreeLibrary(static_cast<HMODULE>(dll_));
        dll_ = nullptr;
    }
}

bool AmfEncoder::submit(ID3D11Texture2D* tex, uint64_t frameIndex, bool forceIdr) {
    if (!encoder_) return false;
    amf::AMFSurface* surface = nullptr;
    if (context_->CreateSurfaceFromDX11Native(tex, &surface, nullptr) != AMF_OK) return false;
    surface->SetProperty(kPropFrameIndex, static_cast<amf_int64>(frameIndex));
    surface->SetProperty(kPropSubmitUs, static_cast<amf_int64>(monotonicMicros()));
    if (forceIdr) {
        if (hevc_) {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
    }
    AMF_RESULT r = encoder_->SubmitInput(surface);
    surface->Release();
    if (r == AMF_INPUT_FULL) {
        log::warn("AMF input full, frame {} skipped", frameIndex);
        return false;
    }
    return r == AMF_OK;
}

std::optional<EncodedFrame> AmfEncoder::poll() {
    if (!encoder_) return std::nullopt;
    amf::AMFData* data = nullptr;
    if (encoder_->QueryOutput(&data) != AMF_OK || !data) return std::nullopt;
    amf::AMFBuffer* buffer = nullptr;
    data->QueryInterface(amf::AMFBuffer::IID(), reinterpret_cast<void**>(&buffer));
    EncodedFrame f;
    amf_int64 v = 0;
    if (data->GetProperty(kPropFrameIndex, &v) == AMF_OK) f.frameIndex = static_cast<uint64_t>(v);
    if (data->GetProperty(kPropSubmitUs, &v) == AMF_OK) f.submitUs = static_cast<uint64_t>(v);
    if (data->GetProperty(hevc_ ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &v) == AMF_OK)
        f.keyframe = hevc_ ? v == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR : v == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
    if (buffer) {
        const auto* p = static_cast<const uint8_t*>(buffer->GetNative());
        f.data.assign(p, p + buffer->GetSize());
        buffer->Release();
    }
    data->Release();
    return f;
}

void AmfEncoder::setBitrate(uint32_t kbps) {
    if (!encoder_) return;
    const amf_int64 bps = static_cast<amf_int64>(kbps) * 1000;
    if (hevc_) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, static_cast<amf_int64>(bps / fps_));
    } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, static_cast<amf_int64>(bps / fps_));
    }
}

}  // namespace kage::driver
