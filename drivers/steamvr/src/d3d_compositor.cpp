#include "d3d_compositor.hpp"

#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include "kage/util/log.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace kage::driver {
namespace {

const char* kShader = R"(
cbuffer Params : register(b0) { float4 uvRect; float4 flags; };  // uv min.xy, max.xy ; flags.x = linear->sRGB
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);       // fullscreen triangle
    VSOut o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = lerp(uvRect.xy, uvRect.zw, p);
    return o;
}
float3 toSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float4 ps(VSOut i) : SV_Target {
    float4 c = tex.Sample(smp, i.uv);
    if (flags.x > 0.5) c.rgb = toSrgb(c.rgb);
    return c;
}
)";

// Textures are created TYPELESS so we can read raw (already gamma-encoded) bytes
// through a UNORM view while the compositor writes through an _SRGB view.
DXGI_FORMAT typelessOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default: return f;
    }
}
DXGI_FORMAT srvOf(DXGI_FORMAT f) {
    switch (typelessOf(f)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}
}  // namespace

bool D3DCompositor::init() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> best, a;
    SIZE_T bestMem = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (d.DedicatedVideoMemory > bestMem) {
            bestMem = d.DedicatedVideoMemory;
            best = a;
        }
    }
    if (!best) return false;
    DXGI_ADAPTER_DESC1 d{};
    best->GetDesc1(&d);
    luid_ = (static_cast<uint64_t>(static_cast<uint32_t>(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &device_, nullptr, &ctx_)))
        return false;
    // Encoder and compositor share this device from several threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
    log::info("D3D11 device on {} ({} MiB)", name, d.DedicatedVideoMemory >> 20);
    return createShaders();
}

bool D3DCompositor::createShaders() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "kage", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), "kage", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err))) {
        log::error("shader compile: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    device_->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);
    D3D11_BLEND_DESC b{};
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&b, &blendOpaque_);
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device_->CreateBlendState(&b, &blendAlpha_);
    return vs_ && ps_ && cb_;
}

bool D3DCompositor::createSet(uint32_t pid, uint32_t w, uint32_t h, uint32_t format, uint32_t samples, uint64_t (&handles)[3]) {
    const auto fmt = static_cast<DXGI_FORMAT>(format);
    Set s;
    s.pid = pid;
    s.linear = srvOf(fmt) == DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = typelessOf(fmt);
    td.SampleDesc.Count = samples ? samples : 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    for (int i = 0; i < 3; ++i) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &s.tex[i]))) {
            log::error("CreateSwapTextureSet {}x{} fmt {} failed", w, h, format);
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srvOf(fmt);
        sv.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(s.tex[i].Get(), &sv, &s.srv[i]);
        ComPtr<IDXGIResource> res;
        HANDLE sh = nullptr;
        s.tex[i].As(&res);
        res->GetSharedHandle(&sh);
        s.handles[i] = reinterpret_cast<uint64_t>(sh);
        handles[i] = s.handles[i];
    }
    std::lock_guard l(mutex_);
    sets_.push_back(std::move(s));
    log::info("swap texture set {}x{} fmt {} samples {} for pid {}", w, h, format, td.SampleDesc.Count, pid);
    return true;
}

const D3DCompositor::Set* D3DCompositor::findByHandle(uint64_t h, int& slot) const {
    for (const auto& s : sets_)
        for (int i = 0; i < 3; ++i)
            if (s.handles[i] == h) {
                slot = i;
                return &s;
            }
    return nullptr;
}

void D3DCompositor::destroySet(uint64_t handle) {
    std::lock_guard l(mutex_);
    std::erase_if(sets_, [&](const Set& s) { return s.handles[0] == handle || s.handles[1] == handle || s.handles[2] == handle; });
}

void D3DCompositor::destroyAllForPid(uint32_t pid) {
    std::lock_guard l(mutex_);
    std::erase_if(sets_, [&](const Set& s) { return s.pid == pid; });
}

uint32_t D3DCompositor::nextIndex(uint64_t handle) {
    std::lock_guard l(mutex_);
    for (auto& s : sets_)
        for (int i = 0; i < 3; ++i)
            if (s.handles[i] == handle) {
                s.index = (static_cast<uint32_t>(i) + 1) % 3;
                return s.index;
            }
    return 0;
}

void D3DCompositor::setOutputSize(uint32_t w, uint32_t h) {
    if (w == outW_ && h == outH_) return;
    outW_ = w;
    outH_ = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < kRing; ++i) {
        device_->CreateTexture2D(&td, nullptr, &out_[i]);
        device_->CreateRenderTargetView(out_[i].Get(), nullptr, &rtv_[i]);
    }
}

ID3D11Texture2D* D3DCompositor::compose(const std::vector<std::array<LayerEye, 2>>& layers, uint64_t syncTexture) {
    if (!outW_ || layers.empty()) return nullptr;
    // SteamVR signals that rendering into the layer textures is finished through the keyed mutex of the sync texture.
    ComPtr<ID3D11Texture2D> sync;
    ComPtr<IDXGIKeyedMutex> km;
    if (syncTexture && SUCCEEDED(device_->OpenSharedResource(reinterpret_cast<HANDLE>(syncTexture), IID_PPV_ARGS(&sync)))) {
        sync.As(&km);
        if (km && km->AcquireSync(0, 10) != S_OK) return nullptr;
    }
    std::lock_guard l(mutex_);
    const int slot = ringPos_;
    ringPos_ = (ringPos_ + 1) % kRing;
    ctx_->OMSetRenderTargets(1, rtv_[slot].GetAddressOf(), nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    for (size_t li = 0; li < layers.size() && li < 4; ++li) {
        ctx_->OMSetBlendState(li == 0 ? blendOpaque_.Get() : blendAlpha_.Get(), nullptr, 0xFFFFFFFF);
        for (int eye = 0; eye < 2; ++eye) {
            int idx = 0;
            const Set* s = findByHandle(layers[li][eye].texture, idx);
            if (!s) continue;
            const auto& e = layers[li][eye];
            const float params[8] = {e.uMin, e.vMin, e.uMax, e.vMax, s->linear ? 1.f : 0.f, 0, 0, 0};
            ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, params, 0, 0);
            D3D11_VIEWPORT vp{static_cast<float>(eye * outW_ / 2), 0, static_cast<float>(outW_ / 2), static_cast<float>(outH_), 0, 1};
            ctx_->RSSetViewports(1, &vp);
            ctx_->PSSetShaderResources(0, 1, s->srv[idx].GetAddressOf());
            ctx_->Draw(3, 0);
        }
    }
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
    ctx_->Flush();
    if (km) km->ReleaseSync(0);
    return out_[slot].Get();
}

}  // namespace kage::driver
