// kage-encbench: measures the PC side of the video latency without a headset.
// AMF encodes a synthetic moving image (detail + noise, scrolled each frame) at the
// stream resolution, paced like the real driver, and reports submit -> last slice
// latency per speed preset and frame rate. Usage: kage-encbench [width height kbps]
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <cstring>
#include <thread>
#include <vector>

#include "amf_encoder.hpp"
#include "kage/util/time.hpp"

using kage::driver::AmfEncoder;

namespace {

struct Result {
    double median = 0, p95 = 0, mbps = 0;
    int frames = 0;
};

Result run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t w, uint32_t h, float fps,
           uint32_t kbps, int preset) {
    AmfEncoder enc;
    enc.wantSlices = true;
    enc.wantIntraRefresh = true;
    enc.preset = preset;
    if (!enc.init(dev, true, w, h, fps, kbps)) return {};
    // Ring of inputs: a texture stays untouched until its frame is out.
    std::vector<ID3D11Texture2D*> ring(4, nullptr);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (auto& t : ring) dev->CreateTexture2D(&td, nullptr, &t);

    constexpr int kWarmup = 30, kFrames = 330;
    std::vector<double> lat;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> bytes{0};
    std::thread out([&] {
        while (!done) {
            auto f = enc.poll();
            if (!f) continue;
            bytes += f->data.size();
            if (f->lastSlice && f->frameIndex >= kWarmup) lat.push_back((kage::monotonicMicros() - f->submitUs) / 1000.0);
        }
    });
    const uint64_t period = static_cast<uint64_t>(1e6 / fps);
    uint64_t next = kage::monotonicMicros();
    uint64_t bytesAtWarmup = 0, tWarm = 0;
    for (int i = 0; i < kFrames; ++i) {
        while (kage::monotonicMicros() < next) std::this_thread::yield();
        next += period;
        auto* t = ring[i % ring.size()];
        D3D11_BOX box{static_cast<UINT>((i * 14) % w), static_cast<UINT>((i * 3) % (h / 4)), 0, 0, 0, 1};
        box.right = box.left + w;
        box.bottom = box.top + h;
        ctx->CopySubresourceRegion(t, 0, 0, 0, 0, src, 0, &box);
        ctx->Flush();
        if (i == kWarmup) {
            bytesAtWarmup = bytes;
            tWarm = kage::monotonicMicros();
        }
        enc.submit(t, static_cast<uint64_t>(i), i == 0);
    }
    Sleep(200);
    done = true;
    out.join();
    const double secs = (kage::monotonicMicros() - 200000 - tWarm) / 1e6;
    enc.shutdown();
    for (auto* t : ring) t->Release();
    Result r;
    r.frames = static_cast<int>(lat.size());
    if (lat.empty()) return r;
    std::sort(lat.begin(), lat.end());
    r.median = lat[lat.size() / 2];
    r.p95 = lat[std::min(lat.size() - 1, lat.size() * 95 / 100)];
    r.mbps = (bytes - bytesAtWarmup) * 8 / secs / 1e6;
    return r;
}

}  // namespace

// Simulated game: another device keeps the GPU busy with a heavy full-screen shader.
void gpuLoad(std::atomic<bool>& stop) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return;
    const char* vs = "float4 main(uint i:SV_VertexID):SV_Position{float2 t=float2((i<<1)&2,i&2);return float4(t*float2(2,-2)+float2(-1,1),0,1);}";
    const char* ps = "float4 main(float4 p:SV_Position):SV_Target{float a=p.x*0.001,b=p.y*0.001;"
                     "[loop]for(int i=0;i<60;i++){a=sin(a+b)*1.01+cos(b);b=cos(a-b)*0.99;}return float4(a,b,a*b,1);}";
    ID3DBlob *vb = nullptr, *pb = nullptr;
    D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, nullptr);
    D3DCompile(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, nullptr);
    ID3D11VertexShader* v = nullptr;
    ID3D11PixelShader* p = nullptr;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &v);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &p);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 3840;
    td.Height = 2160;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);
    D3D11_VIEWPORT vp{0, 0, 3840, 2160, 0, 1};
    ID3D11Query* q = nullptr;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    dev->CreateQuery(&qd, &q);
    while (!stop) {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(v, nullptr, 0);
        ctx->PSSetShader(p, nullptr, 0);
        for (int k = 0; k < 24; ++k) ctx->Draw(3, 0);  // many shorter draws, like a game frame
        ctx->End(q);
        while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE && !stop) std::this_thread::yield();  // ~1 frame queued, like a game
    }
    q->Release();
    rtv->Release();
    rt->Release();
    p->Release();
    v->Release();
    vb->Release();
    pb->Release();
    ctx->Release();
    dev->Release();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "gpuload") {
        std::atomic<bool> stop{false};
        gpuLoad(stop);  // until killed
        return 0;
    }
    const bool loadMode = argc > 1 && std::string(argv[1]) == "charge";
    if (loadMode) --argc, ++argv;
    const uint32_t w = argc > 2 ? std::atoi(argv[1]) : 2832, h = argc > 2 ? std::atoi(argv[2]) : 1488;
    const uint32_t kbps = argc > 3 ? std::atoi(argv[3]) : 100000;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx))) {
        std::puts("D3D11 indisponible");
        return 1;
    }
    // Source twice as large as the frame: smooth shapes + fine detail + noise, like a game.
    const uint32_t sw = w * 2, sh = h + h / 4;
    std::vector<uint32_t> px(static_cast<size_t>(sw) * sh);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> noise(-18, 18);
    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x) {
            const double fx = x / 97.0, fy = y / 83.0;
            auto c = [&](double ph) {
                const double v = 128 + 70 * std::sin(fx + ph) * std::cos(fy - ph) + 30 * std::sin(x / 7.0 + y / 11.0);
                return static_cast<uint32_t>(std::clamp(v + noise(rng), 0.0, 255.0));
            };
            px[static_cast<size_t>(y) * sw + x] = 0xFF000000u | (c(0) << 16) | (c(2.1) << 8) | c(4.2);
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = sw;
    td.Height = sh;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), sw * 4, 0};
    ID3D11Texture2D* src = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &init, &src))) {
        std::puts("texture source impossible");
        return 1;
    }
    if (loadMode) {
        // GPU busy like during a game: encoder latency without / with a raised GPU thread priority.
        std::printf("Charge GPU simulee (jeu), HEVC %ux%u balanced 90 i/s\n", w, h);
        IDXGIDevice* dx = nullptr;
        dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx));
        auto line = [&](const char* label) {
            auto r = run(dev, ctx, src, w, h, 90.f, kbps, 1);
            std::printf("%-28s median %6.2f ms  p95 %6.2f ms\n", label, r.median, r.p95);
            std::fflush(stdout);
        };
        line("sans charge");
        // The "game" runs in its own process, so process-wide GPU priorities only affect us.
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(self) + L"\" gpuload";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        Sleep(1500);
        line("charge, normal");
        const HRESULT hr = dx->SetGPUThreadPriority(7);
        std::printf("SetGPUThreadPriority(7): hr=0x%08lx\n", static_cast<unsigned long>(hr));
        line("charge, thread GPU 7");
        using SetClassFn = LONG(APIENTRY*)(HANDLE, int);
        auto setClass = reinterpret_cast<SetClassFn>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
        for (int c : {4, 5}) {  // HIGH, REALTIME
            const LONG st = setClass ? setClass(GetCurrentProcess(), c) : -1;
            std::printf("D3DKMTSetProcessSchedulingPriorityClass(%s): 0x%08lx\n", c == 4 ? "HIGH" : "REALTIME", static_cast<unsigned long>(st));
            if (st == 0) line(c == 4 ? "charge, processus GPU HIGH" : "charge, processus GPU REALTIME");
        }
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        dx->Release();
        return 0;
    }
    std::printf("AMF HEVC %ux%u, %u Mbit/s, tranches + rafraichissement progressif (comme le pilote)\n", w, h, kbps / 1000);
    std::printf("%-9s %5s %12s %10s %10s\n", "preset", "i/s", "median ms", "p95 ms", "Mbit/s");
    const char* names[] = {"speed", "balanced", "quality"};
    for (float fps : {72.f, 90.f, 120.f})
        for (int p : {2, 1, 0}) {
            auto r = run(dev, ctx, src, w, h, fps, kbps, p);
            if (!r.frames)
                std::printf("%-9s %5.0f   echec\n", names[p], fps);
            else
                std::printf("%-9s %5.0f %12.2f %10.2f %10.1f\n", names[p], fps, r.median, r.p95, r.mbps);
            std::fflush(stdout);
        }
    src->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
