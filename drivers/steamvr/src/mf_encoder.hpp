// Universal encoder through Windows Media Foundation: the GPU vendor's hardware MFT
// (NVIDIA NVENC, Intel Quick Sync, AMD VCN) or, last resort, Microsoft's software H.264
// encoder. RGB -> NV12 conversion on the GPU (D3D11 video processor).
#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "video_encoder.hpp"

namespace kage::driver {

class MfEncoder final : public VideoEncoder {
public:
    MfEncoder(bool allowHardware, bool allowSoftware) : allowHw_(allowHardware), allowSw_(allowSoftware) {}
    ~MfEncoder() override { shutdown(); }
    bool init(ID3D11Device* device, bool hevc, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps) override;
    void shutdown() override;
    bool submit(ID3D11Texture2D* tex, uint64_t frameIndex, bool forceIdr) override;
    std::optional<EncodedFrame> poll() override;
    void setBitrate(uint32_t kbps) override;
    const char* name() const override { return name_.c_str(); }

private:
    bool createTransform(bool hevc, bool hardware);
    bool configure(bool hevc, uint32_t kbps);
    bool convert(ID3D11Texture2D* src, int slot);
    void pump();         // async (hardware) MFT: handle pending events
    bool takeOutput();   // one ProcessOutput; false when nothing more

    bool allowHw_, allowSw_;
    std::string name_ = "Media Foundation";
    std::mutex m_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11VideoDevice* vdev_ = nullptr;
    ID3D11VideoContext* vctx_ = nullptr;
    ID3D11VideoProcessorEnumerator* vpEnum_ = nullptr;
    ID3D11VideoProcessor* vp_ = nullptr;
    std::map<ID3D11Texture2D*, ID3D11VideoProcessorInputView*> inViews_;
    static constexpr int kPool = 4;
    ID3D11Texture2D* nv12_[kPool] = {};
    ID3D11VideoProcessorOutputView* outViews_[kPool] = {};
    ID3D11Texture2D* staging_ = nullptr;  // software path only
    IMFDXGIDeviceManager* devManager_ = nullptr;
    UINT resetToken_ = 0;
    IMFTransform* mft_ = nullptr;
    IMFMediaEventGenerator* events_ = nullptr;
    ICodecAPI* codecApi_ = nullptr;
    bool async_ = false, d3dAware_ = false, hevc_ = true, started_ = false, providesSamples_ = false;
    DWORD inId_ = 0, outId_ = 0;
    uint32_t outBufSize_ = 0, width_ = 0, height_ = 0;
    float fps_ = 72;
    int next_ = 0, needInput_ = 0;
    std::deque<IMFSample*> pending_;
    std::deque<EncodedFrame> out_;
    std::map<int64_t, std::pair<uint64_t, uint64_t>> times_;  // sample time -> (frame index, submit us)
};

}  // namespace kage::driver
