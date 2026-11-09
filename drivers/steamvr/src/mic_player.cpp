#include "mic_player.hpp"

#include <windows.h>
#include <audioclient.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string>

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

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

uint64_t nowMs() { return GetTickCount64(); }

// Playback endpoint whose friendly name contains one of `wanted` (tried in order).
IMMDevice* findDevice(IMMDeviceEnumerator* en, const std::vector<std::wstring>& wanted, std::wstring& name) {
    Com<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0;
    col->GetCount(&n);
    for (const auto& w : wanted)
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            Com<IPropertyStore> ps;
            PROPVARIANT v;
            PropVariantInit(&v);
            const bool match = SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) &&
                               SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR &&
                               lower(v.pwszVal).find(lower(w)) != std::wstring::npos;
            if (match) name = v.pwszVal;
            PropVariantClear(&v);
            if (match) return d;
            d->Release();
        }
    return nullptr;
}
}  // namespace

void MicPlayer::start(const std::filesystem::path& dataDir) {
    if (running_.exchange(true)) return;
    dataDir_ = dataDir;
    thread_ = std::thread(&MicPlayer::loop, this);
}

void MicPlayer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void MicPlayer::push(uint32_t rate, std::span<const int16_t> mono) {
    if (rate != 48000) return;  // the headset always captures at 48 kHz
    std::lock_guard l(m_);
    if (ring_.size() > 48000 / 5) ring_.clear();  // > 200 ms behind: drop, keep latency low
    ring_.insert(ring_.end(), mono.begin(), mono.end());
    lastPushMs_ = nowMs();
}

void MicPlayer::loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_) {
        bool active;
        {
            std::lock_guard l(m_);
            active = nowMs() - lastPushMs_ < 1000;
            if (!active) ring_.clear();
        }
        // No device is opened while the headset mic is off.
        if (!active || !playOnce()) Sleep(active ? 5000 : 100);
    }
    CoUninitialize();
}

// Opens the virtual input and feeds it until the mic stops. false = no usable device.
bool MicPlayer::playOnce() {
    std::vector<std::wstring> wanted;
    if (std::wifstream f(dataDir_ / L"mic_device.txt"); f) {
        std::wstring s;
        std::getline(f, s);
        if (!s.empty()) wanted.push_back(s);
    }
    for (const wchar_t* w : {L"Voicemeeter AUX Input", L"Voicemeeter VAIO3 Input", L"CABLE Input", L"Voicemeeter Input"})
        wanted.push_back(w);
    Com<IMMDeviceEnumerator> en;
    Com<IMMDevice> dev;
    Com<IAudioClient> client;
    Com<IAudioRenderClient> render;
    std::wstring name;
    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) ||
        !(dev.p = findDevice(en.p, wanted, name)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
        FAILED(client->GetMixFormat(&fmt))) {
        log::warn("mic: no virtual input (Voicemeeter / VB-CABLE) found");
        return false;
    }
    const bool isFloat = fmt->wBitsPerSample == 32;
    const UINT ch = fmt->nChannels;
    const double step = 48000.0 / fmt->nSamplesPerSec;  // nearest-sample resampling
    const UINT32 target = fmt->nSamplesPerSec * 40 / 1000;  // ~40 ms queued in the device
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 400'000, 0, fmt, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) ||
        FAILED(client->Start())) {
        CoTaskMemFree(fmt);
        log::warn("mic: cannot open the playback device");
        return false;
    }
    // UTF-8 (the device name may contain accents: "Haut-parleurs", "Câble"...).
    std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    if (!utf8.empty()) utf8.pop_back();  // terminating NUL
    log::info("mic: headset microphone -> {}", utf8);
    UINT32 bufFrames = 0;
    client->GetBufferSize(&bufFrames);
    double pos = 0;
    while (running_) {
        Sleep(5);
        std::lock_guard l(m_);
        if (nowMs() - lastPushMs_ > 1000) break;  // mic turned off on the headset
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) break;
        const auto avail = static_cast<UINT32>((static_cast<double>(ring_.size()) - pos) / step);
        const UINT32 frames = std::min({bufFrames - pad, target > pad ? target - pad : 0u, avail});
        if (!frames) continue;
        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(frames, &out))) break;
        for (UINT32 i = 0; i < frames; ++i, pos += step) {
            const float s = ring_[static_cast<size_t>(pos)] / 32768.f;
            for (UINT c = 0; c < ch; ++c) {
                if (isFloat) reinterpret_cast<float*>(out)[i * ch + c] = s;
                else reinterpret_cast<int16_t*>(out)[i * ch + c] = static_cast<int16_t>(s * 32767.f);
            }
        }
        render->ReleaseBuffer(frames, 0);
        const auto used = static_cast<size_t>(pos);
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(used));
        pos -= static_cast<double>(used);
    }
    client->Stop();
    CoTaskMemFree(fmt);
    log::info("mic: stopped");
    return true;
}

}  // namespace kage::driver
