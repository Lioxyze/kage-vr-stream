// Common interface of the hardware video encoders (AMD AMF, Media Foundation).
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace kage::driver {

struct EncodedFrame {
    uint64_t frameIndex = 0;
    bool keyframe = false;
    uint64_t submitUs = 0;
    std::vector<uint8_t> data;  // Annex B
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual bool init(ID3D11Device* device, bool hevc, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps) = 0;
    virtual void shutdown() = 0;
    virtual bool submit(ID3D11Texture2D* tex, uint64_t frameIndex, bool forceIdr) = 0;
    virtual std::optional<EncodedFrame> poll() = 0;
    virtual void setBitrate(uint32_t kbps) = 0;
    virtual const char* name() const = 0;
};

}  // namespace kage::driver
