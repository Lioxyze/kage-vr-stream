#include "audio_capture.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "kage/util/log.hpp"

namespace kage::driver {

namespace {
template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

int16_t toS16(float f) {
    f = std::clamp(f, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrintf(f * 32767.0f));
}
}  // namespace

void AudioCapture::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&AudioCapture::loop, this);
}

void AudioCapture::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::loop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<int16_t> out;
    while (running_) {
        if (enabled && !enabled()) {
            Sleep(200);
            continue;
        }
        // One capture session; re-created when the default device changes or fails.
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        Com<IAudioClient> client;
        Com<IAudioCaptureClient> cap;
        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&en))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
            FAILED(client->GetMixFormat(&fmt))) {
            log::warn("audio: no default output device");
            Sleep(2000);
            continue;
        }
        bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        const bool isS16 = !isFloat && fmt->wBitsPerSample == 16;
        const uint32_t rate = fmt->nSamplesPerSec;
        const uint32_t ch = fmt->nChannels;
        // 20 ms shared-mode buffer; loopback is polled every ~2 ms.
        const HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200'000, 0, fmt, nullptr);
        CoTaskMemFree(fmt);
        if ((!isFloat && !isS16) || FAILED(hr) ||
            FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap))) || FAILED(client->Start())) {
            log::warn("audio: loopback capture unavailable (hr=0x{:08x})", static_cast<uint32_t>(hr));
            Sleep(2000);
            continue;
        }
        log::info("audio: capturing system output {} Hz, {} ch", rate, ch);
        const size_t maxFrames = rate / 200;  // 5 ms per packet: 960 B at 48 kHz
        bool ok = true;
        while (running_ && ok && (!enabled || enabled())) {
            UINT32 avail = 0;
            if (FAILED(cap->GetNextPacketSize(&avail))) break;
            if (avail == 0) {
                Sleep(2);
                continue;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            out.resize(static_cast<size_t>(frames) * 2);
            const bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
            for (UINT32 i = 0; i < frames; ++i) {
                for (uint32_t c = 0; c < 2; ++c) {
                    const uint32_t src = std::min(c, ch - 1);  // mono -> both ears, >2 ch -> front L/R
                    int16_t v = 0;
                    if (!silent) {
                        if (isFloat) v = toS16(reinterpret_cast<const float*>(data)[i * ch + src]);
                        else v = reinterpret_cast<const int16_t*>(data)[i * ch + src];
                    }
                    out[i * 2 + c] = v;
                }
            }
            cap->ReleaseBuffer(frames);
            for (size_t off = 0; off < frames && onChunk; off += maxFrames) {
                const size_t n = std::min(maxFrames, frames - off);
                onChunk(rate, {out.data() + off * 2, n * 2});
            }
        }
        client->Stop();
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace kage::driver
