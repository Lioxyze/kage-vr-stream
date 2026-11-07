#include "mf_encoder.hpp"

#include <windows.h>
#include <codecapi.h>
#include <d3d10.h>
#include <mferror.h>
#include <strmif.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace kage::driver {
namespace {
template <class T>
void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void setUi4(ICodecAPI* api, const GUID& key, ULONG v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&key, &var);
}

void setBool(ICodecAPI* api, const GUID& key, bool v) {
    if (!api) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &var);
}

// Type of the first NAL unit of an Annex B buffer (-1 if none).
int firstNal(const std::vector<uint8_t>& d, bool hevc) {
    for (size_t i = 0; i + 3 < d.size(); ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return hevc ? (d[i + 3] >> 1) & 0x3F : d[i + 3] & 0x1F;
    return -1;
}

struct MtLock {  // the D3D11 context is shared with the compositor
    ID3D10Multithread* mt = nullptr;
    explicit MtLock(ID3D11DeviceContext* c) {
        if (c && SUCCEEDED(c->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) mt->Enter();
    }
    ~MtLock() {
        if (mt) {
            mt->Leave();
            mt->Release();
        }
    }
};
}  // namespace

bool MfEncoder::init(ID3D11Device* device, bool hevc, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps) {
    shutdown();
    device_ = device;
    device_->AddRef();
    device_->GetImmediateContext(&ctx_);
    width_ = width;
    height_ = height;
    fps_ = fps;
    hevc_ = hevc;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    started_ = true;

    // GPU colour conversion RGB -> NV12.
    if (FAILED(device_->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&vdev_))) ||
        FAILED(ctx_->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&vctx_)))) {
        log::error("mf: no D3D11 video processor on this GPU");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = cd.OutputWidth = width;
    cd.InputHeight = cd.OutputHeight = height;
    cd.InputFrameRate = cd.OutputFrameRate = {static_cast<UINT>(std::lround(fps)), 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_)) || FAILED(vdev_->CreateVideoProcessor(vpEnum_, 0, &vp_)))
        return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    for (auto& t : nv12_)
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &t))) return false;

    if (FAILED(MFCreateDXGIDeviceManager(&resetToken_, &devManager_)) || FAILED(devManager_->ResetDevice(device_, resetToken_)))
        return false;

    const uint32_t kbps = bitrateKbps;
    if (!(allowHw_ && createTransform(hevc, true) && configure(hevc, kbps))) {
        rel(mft_);
        rel(events_);
        rel(codecApi_);
        if (!(allowSw_ && !hevc && createTransform(false, false) && configure(false, kbps))) {
            log::error("mf: no {} encoder available (hardware{})", hevc ? "HEVC" : "H.264", hevc ? "" : " or software");
            return false;
        }
    }
    if (!d3dAware_) {  // software encoder: CPU copy through a staging texture
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) return false;
    }
    log::info("mf: encoder '{}' {} {}x{} @ {:.0f} Hz, {} kbit/s{}", name_, hevc ? "HEVC" : "H.264", width, height, fps, kbps,
              async_ ? " (hardware)" : " (software)");
    return true;
}

bool MfEncoder::createTransform(bool hevc, bool hardware) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &count)) || !count) return false;
    bool ok = false;
    for (UINT32 i = 0; i < count && !ok; ++i) {
        wchar_t* fname = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &len))) {
            name_.clear();
            for (UINT32 k = 0; k < len; ++k) name_ += static_cast<char>(fname[k] < 128 ? fname[k] : '?');
            CoTaskMemFree(fname);
        }
        if (FAILED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft_)))) continue;
        IMFAttributes* attrs = nullptr;
        async_ = d3dAware_ = false;
        if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
            async_ = MFGetAttributeUINT32(attrs, MF_TRANSFORM_ASYNC, FALSE) != 0;
            if (async_) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            d3dAware_ = MFGetAttributeUINT32(attrs, MF_SA_D3D11_AWARE, FALSE) != 0;
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            attrs->Release();
        }
        if (d3dAware_ &&
            FAILED(mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(devManager_))))
            d3dAware_ = false;
        if (hardware && !d3dAware_) {  // a hardware MFT that cannot take GPU textures: skip it
            rel(mft_);
            continue;
        }
        ok = true;
    }
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return ok;
}

bool MfEncoder::configure(bool hevc, uint32_t kbps) {
    if (!mft_) return false;
    DWORD ni = 0, no = 0;
    mft_->GetStreamCount(&ni, &no);
    if (mft_->GetStreamIDs(1, &inId_, 1, &outId_) == E_NOTIMPL) inId_ = outId_ = 0;
    mft_->QueryInterface(IID_PPV_ARGS(&codecApi_));
    setUi4(codecApi_, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    setUi4(codecApi_, CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
    setBool(codecApi_, CODECAPI_AVLowLatencyMode, true);
    setUi4(codecApi_, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    setUi4(codecApi_, CODECAPI_AVEncMPVGOPSize, 0x7FFFFFFF);  // keyframes only on demand
    setUi4(codecApi_, CODECAPI_AVEncCommonQualityVsSpeed, 50);

    IMFMediaType* outT = nullptr;
    MFCreateMediaType(&outT);
    outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outT->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    outT->SetUINT32(MF_MT_AVG_BITRATE, kbps * 1000);
    MFSetAttributeSize(outT, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(outT, MF_MT_FRAME_RATE, static_cast<UINT32>(std::lround(fps_)), 1);
    MFSetAttributeRatio(outT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outT->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_Main);
    HRESULT hr = mft_->SetOutputType(outId_, outT, 0);
    outT->Release();
    if (FAILED(hr)) return false;

    IMFMediaType* inT = nullptr;
    MFCreateMediaType(&inT);
    inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inT, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(inT, MF_MT_FRAME_RATE, static_cast<UINT32>(std::lround(fps_)), 1);
    MFSetAttributeRatio(inT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = mft_->SetInputType(inId_, inT, 0);
    inT->Release();
    if (FAILED(hr)) return false;

    MFT_OUTPUT_STREAM_INFO si{};
    mft_->GetOutputStreamInfo(outId_, &si);
    providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outBufSize_ = std::max<uint32_t>(si.cbSize, width_ * height_);
    if (async_ && FAILED(mft_->QueryInterface(IID_PPV_ARGS(&events_)))) return false;
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void MfEncoder::shutdown() {
    std::lock_guard l(m_);
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    for (auto* s : pending_) s->Release();
    pending_.clear();
    out_.clear();
    times_.clear();
    rel(codecApi_);
    rel(events_);
    rel(mft_);
    rel(devManager_);
    for (auto& t : nv12_) rel(t);
    rel(staging_);
    for (auto& [tex, v] : inViews_) v->Release();
    inViews_.clear();
    for (auto& v : outViews_) rel(v);
    rel(vp_);
    rel(vpEnum_);
    rel(vctx_);
    rel(vdev_);
    rel(ctx_);
    rel(device_);
    if (started_) MFShutdown();
    started_ = false;
    needInput_ = 0;
}

bool MfEncoder::convert(ID3D11Texture2D* src, int slot) {
    ID3D11VideoProcessorInputView*& in = inViews_[src];
    if (!in) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev_->CreateVideoProcessorInputView(src, vpEnum_, &d, &in))) {
            inViews_.erase(src);
            return false;
        }
    }
    if (!outViews_[slot]) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev_->CreateVideoProcessorOutputView(nv12_[slot], vpEnum_, &d, &outViews_[slot]))) return false;
    }
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = in;
    return SUCCEEDED(vctx_->VideoProcessorBlt(vp_, outViews_[slot], 0, 1, &st));
}

bool MfEncoder::submit(ID3D11Texture2D* tex, uint64_t frameIndex, bool forceIdr) {
    std::lock_guard l(m_);
    if (!mft_) return false;
    const int slot = next_++ % kPool;
    IMFMediaBuffer* buf = nullptr;
    {
        MtLock lock(ctx_);
        if (!convert(tex, slot)) return false;
        if (d3dAware_) {
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_[slot], 0, FALSE, &buf))) return false;
            IMF2DBuffer* b2 = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2)))) {
                b2->GetContiguousLength(&len);
                b2->Release();
            }
            buf->SetCurrentLength(len);
        } else {
            ctx_->CopyResource(staging_, nv12_[slot]);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m))) return false;
            const DWORD size = width_ * height_ * 3 / 2;
            BYTE* dst = nullptr;
            if (SUCCEEDED(MFCreateMemoryBuffer(size, &buf)) && SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
                const auto* srcp = static_cast<const BYTE*>(m.pData);
                for (uint32_t y = 0; y < height_; ++y) memcpy(dst + y * width_, srcp + y * m.RowPitch, width_);
                const BYTE* uv = srcp + static_cast<size_t>(m.RowPitch) * height_;
                for (uint32_t y = 0; y < height_ / 2; ++y)
                    memcpy(dst + width_ * height_ + y * width_, uv + y * m.RowPitch, width_);
                buf->Unlock();
                buf->SetCurrentLength(size);
            }
            ctx_->Unmap(staging_, 0);
            if (!buf) return false;
        }
    }
    IMFSample* s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(buf);
    buf->Release();
    const int64_t dur = static_cast<int64_t>(10'000'000 / std::max(1.f, fps_));
    const int64_t t = static_cast<int64_t>(frameIndex) * dur;
    s->SetSampleTime(t);
    s->SetSampleDuration(dur);
    times_[t] = {frameIndex, monotonicMicros()};
    if (times_.size() > 64) times_.erase(times_.begin());
    if (forceIdr) setUi4(codecApi_, CODECAPI_AVEncVideoForceKeyFrame, 1);
    if (async_) {
        pending_.push_back(s);
        pump();
    } else {
        const HRESULT hr = mft_->ProcessInput(inId_, s, 0);
        s->Release();
        if (FAILED(hr)) return false;
        while (takeOutput()) {
        }
    }
    return true;
}

void MfEncoder::pump() {
    if (!events_) return;
    for (;;) {
        // Feed queued frames as soon as the encoder asks for input.
        while (needInput_ > 0 && !pending_.empty()) {
            IMFSample* s = pending_.front();
            pending_.pop_front();
            mft_->ProcessInput(inId_, s, 0);
            s->Release();
            --needInput_;
        }
        while (pending_.size() > 2) {  // encoder too slow: drop the oldest (latency first)
            pending_.front()->Release();
            pending_.pop_front();
        }
        IMFMediaEvent* ev = nullptr;
        if (events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev) != S_OK || !ev) return;
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        ev->Release();
        if (type == METransformNeedInput) ++needInput_;
        else if (type == METransformHaveOutput) takeOutput();
    }
}

bool MfEncoder::takeOutput() {
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = outId_;
    if (!providesSamples_) {
        IMFMediaBuffer* b = nullptr;
        MFCreateSample(&ob.pSample);
        MFCreateMemoryBuffer(outBufSize_, &b);
        ob.pSample->AddBuffer(b);
        b->Release();
    }
    DWORD status = 0;
    const HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {  // re-accept the (new) output type
        IMFMediaType* t = nullptr;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outId_, 0, &t))) {
            mft_->SetOutputType(outId_, t, 0);
            t->Release();
        }
        if (ob.pSample) ob.pSample->Release();
        return true;
    }
    if (FAILED(hr) || !ob.pSample) {
        if (ob.pSample) ob.pSample->Release();
        return false;
    }
    EncodedFrame f;
    IMFMediaBuffer* cb = nullptr;
    if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&cb))) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(cb->Lock(&p, nullptr, &len))) {
            f.data.assign(p, p + len);
            cb->Unlock();
        }
        cb->Release();
    }
    f.keyframe = MFGetAttributeUINT32(ob.pSample, MFSampleExtension_CleanPoint, FALSE) != 0;
    LONGLONG t = 0;
    ob.pSample->GetSampleTime(&t);
    ob.pSample->Release();
    if (auto it = times_.find(t); it != times_.end()) {
        f.frameIndex = it->second.first;
        f.submitUs = it->second.second;
        times_.erase(times_.begin(), std::next(it));
    } else {
        f.submitUs = monotonicMicros();
    }
    // Some encoders keep SPS/PPS(/VPS) only in the media type: prepend them to keyframes.
    if (f.keyframe) {
        const int nal = firstNal(f.data, hevc_);
        const bool hasHeaders = hevc_ ? (nal == 32 || nal == 33 || nal == 34 || nal == 35) : (nal == 7 || nal == 9);
        IMFMediaType* cur = nullptr;
        if (!hasHeaders && SUCCEEDED(mft_->GetOutputCurrentType(outId_, &cur))) {
            UINT8* blob = nullptr;
            UINT32 size = 0;
            if (SUCCEEDED(cur->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &size))) {
                f.data.insert(f.data.begin(), blob, blob + size);
                CoTaskMemFree(blob);
            }
            cur->Release();
        }
    }
    if (!f.data.empty()) out_.push_back(std::move(f));
    return true;
}

std::optional<EncodedFrame> MfEncoder::poll() {
    for (int i = 0; i < 10; ++i) {
        {
            std::lock_guard l(m_);
            if (!mft_) return std::nullopt;
            pump();
            if (!out_.empty()) {
                EncodedFrame f = std::move(out_.front());
                out_.pop_front();
                return f;
            }
        }
        Sleep(1);
    }
    return std::nullopt;
}

void MfEncoder::setBitrate(uint32_t kbps) {
    std::lock_guard l(m_);
    setUi4(codecApi_, CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
}

}  // namespace kage::driver
