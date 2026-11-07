// AMD AMF hardware HEVC/H.264 encoder configured for ultra-low latency.
// Input: D3D11 BGRA textures on the same device (no CPU copy, the VCN block
// performs the RGB->NV12 conversion itself).
#pragma once

#include "video_encoder.hpp"

namespace amf {
class AMFFactory;
class AMFContext;
class AMFComponent;
}  // namespace amf

namespace kage::driver {

class AmfEncoder final : public VideoEncoder {
public:
    ~AmfEncoder() override;
    bool init(ID3D11Device* device, bool hevc, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps) override;
    void shutdown() override;
    const char* name() const override { return "AMD AMF"; }

    // Non-blocking: queues the texture for encoding. The texture must stay untouched until its frame is output.
    bool submit(ID3D11Texture2D* tex, uint64_t frameIndex, bool forceIdr) override;
    // Waits up to the configured query timeout (~10 ms) for an output.
    std::optional<EncodedFrame> poll() override;
    void setBitrate(uint32_t kbps) override;

private:
    void* dll_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* encoder_ = nullptr;
    bool hevc_ = true;
    float fps_ = 72;
};

}  // namespace kage::driver
