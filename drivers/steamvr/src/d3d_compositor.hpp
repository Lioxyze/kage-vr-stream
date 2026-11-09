// D3D11 side of the direct-mode HMD: owns the swap texture sets SteamVR
// renders into, and composites the submitted layers (both eyes side by side)
// into a ring of BGRA textures handed to the encoder. GPU only, no readback.
#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kage::driver {

using Microsoft::WRL::ComPtr;

struct LayerEye {
    uint64_t texture = 0;  // shared handle
    float uMin = 0, vMin = 0, uMax = 1, vMax = 1;
};

class D3DCompositor {
public:
    static constexpr int kRing = 4;

    bool init();  // creates the device on the highest-VRAM hardware adapter
    uint64_t adapterLuid() const { return luid_; }
    ID3D11Device* device() const { return device_.Get(); }

    // Swap texture set management (called by SteamVR through the HMD).
    bool createSet(uint32_t pid, uint32_t width, uint32_t height, uint32_t format, uint32_t samples, uint64_t (&handles)[3]);
    void destroySet(uint64_t anyHandle);
    void destroyAllForPid(uint32_t pid);
    uint32_t nextIndex(uint64_t handle);

    void setOutputSize(uint32_t w, uint32_t h);
    void setFoveation(float a) { foveationA_ = a; }  // 1 = off (see protocol::foveationA)
    // Composites up to 4 layers (first opaque, others alpha-blended) into the next ring slot.
    // Returns the texture to encode, or nullptr on failure.
    ID3D11Texture2D* compose(const std::vector<std::array<LayerEye, 2>>& layers, uint64_t syncTexture);

private:
    struct Set {
        uint32_t pid = 0;
        uint32_t index = 0;
        bool linear = false;  // float formats hold linear light and need sRGB encoding
        std::array<ComPtr<ID3D11Texture2D>, 3> tex;
        std::array<ComPtr<ID3D11ShaderResourceView>, 3> srv;
        std::array<uint64_t, 3> handles{};
    };
    bool createShaders();
    const Set* findByHandle(uint64_t h, int& slot) const;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    uint64_t luid_ = 0;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendOpaque_, blendAlpha_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D10Multithread> mt_;
    std::array<ComPtr<ID3D11Texture2D>, kRing> out_;
    std::array<ComPtr<ID3D11RenderTargetView>, kRing> rtv_;
    uint32_t outW_ = 0, outH_ = 0;
    float foveationA_ = 1.f;
    int ringPos_ = 0;
    uint64_t syncHandle_ = 0;
    ComPtr<IDXGIKeyedMutex> syncKm_;
    uint32_t syncFailures_ = 0;
    uint64_t composed_ = 0;
    std::optional<std::wstring> dumpPath_;
    void dump(ID3D11Texture2D* tex);

public:
    void enableDump(const std::wstring& path) { dumpPath_ = path; }
    std::mutex mutex_;
    std::vector<Set> sets_;
};

}  // namespace kage::driver
