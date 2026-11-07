// KAGE VR Stream - SteamVR (OpenVR) driver.
// Loaded by vrserver.exe. Exposes one HMD (direct mode, frames composited on
// the GPU and encoded with AMF) and two Touch-compatible controllers whose
// poses and inputs come from the headset over UDP.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <mutex>
#include <thread>

#include "amf_encoder.hpp"
#include "audio_capture.hpp"
#ifdef KAGE_WITH_RIFT
#include "rift_tracking.hpp"
#endif
#include "d3d_compositor.hpp"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"
#include "openvr_driver.h"
#include "stream_server.hpp"

namespace kp = kage::protocol;
namespace klog = kage::log;

namespace {

vr::HmdQuaternion_t toVr(const std::array<float, 4>& q) { return {q[3], q[0], q[1], q[2]}; }

std::array<float, 4> quatFromMatrix(const vr::HmdMatrix34_t& m) {
    const float t = m.m[0][0] + m.m[1][1] + m.m[2][2];
    std::array<float, 4> q;  // x y z w
    if (t > 0) {
        const float s = std::sqrt(t + 1.f) * 2.f;
        q = {(m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s, 0.25f * s};
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f;
        q = {0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s};
    } else if (m.m[1][1] > m.m[2][2]) {
        const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f;
        q = {(m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s};
    } else {
        const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f;
        q = {(m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s};
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (auto& v : q) v /= n;
    return q;
}

vr::DriverPose_t makePose(const kp::Pose& p, bool valid, bool connected) {
    vr::DriverPose_t d{};
    d.poseIsValid = valid;
    d.deviceIsConnected = connected;
    d.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    d.qWorldFromDriverRotation.w = 1;
    d.qDriverFromHeadRotation.w = 1;
    for (int i = 0; i < 3; ++i) d.vecPosition[i] = p.position[i];
    d.qRotation = toVr(p.orientation);
    // The headset already predicted the pose for the expected display time;
    // zero velocities stop SteamVR from extrapolating a second time.
    d.poseTimeOffset = 0;
    return d;
}

// The headset sends the OpenXR *grip* pose; SteamVR's Touch profile (and games'
// hand models) expect the native Oculus controller frame, which is tilted and
// shifted relative to grip. Default offset (left hand; x mirrored for the right):
// derived from SteamVR's own render model data. Tunable live without rebuilding through
// %LOCALAPPDATA%\KageVR\controller_offset.txt: "x y z pitchDeg" (meters, grip frame).
struct ControllerOffset {
    // Inverse of the "openxr_grip" component of SteamVR's oculus_quest_plus_controller
    // render model (grip at (0.007,-0.0018,0.1019), 20.6 deg about X in model space).
    // x then tuned by eye on a Quest 3S (+1 cm towards the other hand).
    float x = 0.003f, y = -0.0342f, z = -0.0961f, pitchDeg = -20.6f;
};
std::mutex g_offsetMutex;
ControllerOffset g_offset;

void reloadControllerOffset(const std::filesystem::path& file) {
    ControllerOffset o;
    if (FILE* f = _wfopen(file.c_str(), L"r")) {
        ControllerOffset r;
        if (std::fscanf(f, "%f %f %f %f", &r.x, &r.y, &r.z, &r.pitchDeg) == 4 && std::fabs(r.x) < 0.3f &&
            std::fabs(r.y) < 0.3f && std::fabs(r.z) < 0.3f && std::fabs(r.pitchDeg) <= 180.f)
            o = r;
        std::fclose(f);
    }
    std::lock_guard l(g_offsetMutex);
    g_offset = o;
}

void applyControllerOffset(vr::DriverPose_t& d, kp::Hand hand) {
    ControllerOffset o;
    {
        std::lock_guard l(g_offsetMutex);
        o = g_offset;
    }
    const float h = o.pitchDeg * 3.14159265f / 360.f;
    d.qDriverFromHeadRotation = {std::cos(h), std::sin(h), 0, 0};  // w x y z: rotation about local X
    d.vecDriverFromHeadTranslation[0] = hand == kp::Hand::Left ? o.x : -o.x;
    d.vecDriverFromHeadTranslation[1] = o.y;
    d.vecDriverFromHeadTranslation[2] = o.z;
}

// ---------------------------------------------------------------------------
class Controller : public vr::ITrackedDeviceServerDriver {
public:
    explicit Controller(kp::Hand hand) : hand_(hand) {}
    const char* serial() const { return hand_ == kp::Hand::Left ? "KAGE-CTRL-L" : "KAGE-CTRL-R"; }
    vr::PropertyContainerHandle_t container() const { return container_; }
    vr::VRInputComponentHandle_t haptic() const { return haptic_; }
    kp::Hand hand() const { return hand_; }

    vr::EVRInitError Activate(uint32_t id) override {
        id_ = id;
        auto* props = vr::VRProperties();
        container_ = props->TrackedDeviceToPropertyContainer(id);
        const bool left = hand_ == kp::Hand::Left;
        props->SetInt32Property(container_, vr::Prop_ControllerRoleHint_Int32,
                                left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        // Present as Oculus Touch so every game gets its existing Touch bindings.
        props->SetStringProperty(container_, vr::Prop_ControllerType_String, "oculus_touch");
        props->SetStringProperty(container_, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
        props->SetStringProperty(container_, vr::Prop_RenderModelName_String,
                                 left ? "oculus_quest_plus_controller_left" : "oculus_quest_plus_controller_right");
        props->SetStringProperty(container_, vr::Prop_ManufacturerName_String, "Oculus");
        props->SetStringProperty(container_, vr::Prop_TrackingSystemName_String, "oculus");
        props->SetStringProperty(container_, vr::Prop_ModelNumber_String,
                                 left ? "Miramar (Left Controller)" : "Miramar (Right Controller)");
        props->SetStringProperty(container_, vr::Prop_SerialNumber_String, serial());
        props->SetInt32Property(container_, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);

        auto* in = vr::VRDriverInput();
        auto b = [&](const char* path, vr::VRInputComponentHandle_t& h) { in->CreateBooleanComponent(container_, path, &h); };
        auto s1 = [&](const char* path, vr::VRInputComponentHandle_t& h) {
            in->CreateScalarComponent(container_, path, &h, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        };
        auto s2 = [&](const char* path, vr::VRInputComponentHandle_t& h) {
            in->CreateScalarComponent(container_, path, &h, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided);
        };
        b(left ? "/input/x/click" : "/input/a/click", h_.btn1);
        b(left ? "/input/x/touch" : "/input/a/touch", h_.btn1Touch);
        b(left ? "/input/y/click" : "/input/b/click", h_.btn2);
        b(left ? "/input/y/touch" : "/input/b/touch", h_.btn2Touch);
        b("/input/system/click", h_.system);
        b("/input/trigger/click", h_.triggerClick);
        b("/input/trigger/touch", h_.triggerTouch);
        s1("/input/trigger/value", h_.trigger);
        s1("/input/grip/value", h_.grip);
        b("/input/grip/touch", h_.gripTouch);
        b("/input/joystick/click", h_.stickClick);
        b("/input/joystick/touch", h_.stickTouch);
        s2("/input/joystick/x", h_.stickX);
        s2("/input/joystick/y", h_.stickY);
        b("/input/thumbrest/touch", h_.thumbrest);
        in->CreateHapticComponent(container_, "/output/haptic", &haptic_);
        return vr::VRInitError_None;
    }
    void Deactivate() override { id_ = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* r, uint32_t n) override {
        if (n) r[0] = 0;
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard l(m_);
        return pose_;
    }

    void update(const kp::ControllerState& c) {
        if (id_ == vr::k_unTrackedDeviceIndexInvalid) return;
        const bool connected = c.flags & kp::kConnected;
        {
            std::lock_guard l(m_);
            pose_ = makePose(c.pose, connected && (c.flags & kp::kPoseValid), connected);
            applyControllerOffset(pose_, hand_);
        }
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id_, pose_, sizeof(vr::DriverPose_t));
        auto* in = vr::VRDriverInput();
        const bool left = hand_ == kp::Hand::Left;
        const uint32_t b1 = left ? kp::kButtonX : kp::kButtonA, b2 = left ? kp::kButtonY : kp::kButtonB;
        in->UpdateBooleanComponent(h_.btn1, c.buttons & b1, 0);
        in->UpdateBooleanComponent(h_.btn1Touch, c.touches & b1, 0);
        in->UpdateBooleanComponent(h_.btn2, c.buttons & b2, 0);
        in->UpdateBooleanComponent(h_.btn2Touch, c.touches & b2, 0);
        in->UpdateBooleanComponent(h_.system, c.buttons & kp::kButtonMenu, 0);
        in->UpdateBooleanComponent(h_.triggerClick, c.trigger > 0.75f, 0);
        in->UpdateBooleanComponent(h_.triggerTouch, c.touches & kp::kButtonTrigger, 0);
        in->UpdateScalarComponent(h_.trigger, c.trigger, 0);
        in->UpdateScalarComponent(h_.grip, c.grip, 0);
        in->UpdateBooleanComponent(h_.gripTouch, c.grip > 0.05f, 0);
        in->UpdateBooleanComponent(h_.stickClick, c.buttons & kp::kButtonThumbstick, 0);
        in->UpdateBooleanComponent(h_.stickTouch, c.touches & kp::kButtonThumbstick, 0);
        in->UpdateScalarComponent(h_.stickX, c.thumbstickX, 0);
        in->UpdateScalarComponent(h_.stickY, c.thumbstickY, 0);
        in->UpdateBooleanComponent(h_.thumbrest, c.touches & kp::kButtonThumbrest, 0);
    }

private:
    kp::Hand hand_;
    uint32_t id_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::PropertyContainerHandle_t container_ = 0;
    vr::VRInputComponentHandle_t haptic_ = 0;
    struct {
        vr::VRInputComponentHandle_t btn1, btn1Touch, btn2, btn2Touch, system, triggerClick, triggerTouch, trigger, grip,
            gripTouch, stickClick, stickTouch, stickX, stickY, thumbrest;
    } h_{};
    std::mutex m_;
    vr::DriverPose_t pose_ = makePose({}, false, false);
};

// ---------------------------------------------------------------------------
class Hmd : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent, public vr::IVRDriverDirectModeComponent {
public:
    Hmd(kage::driver::StreamServer& net, const kp::StreamHello& hello, const kp::StreamConfig& cfg)
        : net_(net), hello_(hello), cfg_(cfg) {}

    bool initGraphics() {
        if (!gpu_.init()) return false;
        wchar_t dumpEnv[8];
        wchar_t local[MAX_PATH];
        // Debug capture: env var, or a "dump.request" file in the data folder.
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) &&
            (GetEnvironmentVariableW(L"KAGE_DUMP_FRAME", dumpEnv, 8) ||
             std::filesystem::exists(std::wstring(local) + L"\\KageVR\\dump.request")))
            gpu_.enableDump(std::wstring(local) + L"\\KageVR\\frame.bmp");
        gpu_.setOutputSize(cfg_.width, cfg_.height);
        bitrateKbps_ = cfg_.bitrateKbps;  // start at the chosen quality; AIMD backs off on loss
        if (!encoder_.init(gpu_.device(), cfg_.codec == kp::kCodecHEVC, cfg_.width, cfg_.height, cfg_.refreshHz, bitrateKbps_))
            return false;
        return true;
    }

    vr::EVRInitError Activate(uint32_t id) override {
        id_ = id;
        auto* p = vr::VRProperties();
        auto c = p->TrackedDeviceToPropertyContainer(id);
        p->SetStringProperty(c, vr::Prop_ModelNumber_String, "KAGE Quest");
        p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "KAGE");
        p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
        p->SetStringProperty(c, vr::Prop_SerialNumber_String, "KAGE-HMD");
        p->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, hello_.ipdMeters);
        p->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
        p->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, cfg_.refreshHz);
        // Poses are already predicted to photon time on the headset.
        p->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.f);
        p->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
        p->SetBoolProperty(c, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
        p->SetBoolProperty(c, vr::Prop_HasDriverDirectModeComponent_Bool, true);
        p->SetUint64Property(c, vr::Prop_GraphicsAdapterLuid_Uint64, gpu_.adapterLuid());
        p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        running_ = true;
        vsync_ = std::thread(&Hmd::vsyncLoop, this);
        encodeOut_ = std::thread(&Hmd::encoderLoop, this);
        klog::info("HMD activated: {}x{} per eye, {:.0f} Hz, IPD {:.1f} mm", hello_.eyeWidth, hello_.eyeHeight, cfg_.refreshHz,
                   hello_.ipdMeters * 1000);
        return vr::VRInitError_None;
    }
    void Deactivate() override {
        running_ = false;
        if (vsync_.joinable()) vsync_.join();
        if (encodeOut_.joinable()) encodeOut_.join();
        encoder_.shutdown();
        id_ = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void* GetComponent(const char* name) override {
        if (!std::strcmp(name, vr::IVRDisplayComponent_Version)) return static_cast<vr::IVRDisplayComponent*>(this);
        if (!std::strcmp(name, vr::IVRDriverDirectModeComponent_Version)) return static_cast<vr::IVRDriverDirectModeComponent*>(this);
        return nullptr;
    }
    void DebugRequest(const char*, char* r, uint32_t n) override {
        if (n) r[0] = 0;
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard l(poseMutex_);
        return pose_;
    }

    void update(const kp::TrackingState& t) {
        if (id_ == vr::k_unTrackedDeviceIndexInvalid) return;
        ++trackingCount_;
        {
            std::lock_guard l(poseMutex_);
            pose_ = makePose(t.head, t.headFlags & kp::kPoseValid, true);
            lastTrackingUs_ = t.sampleTimeUs;
        }
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id_, pose_, sizeof(vr::DriverPose_t));
    }
    void requestIdr() { idr_ = true; }

    // Deterministic AIMD bitrate control from the headset's 1 Hz report.
    // Congestion = lost frames, fewer frames than the refresh rate, or one-way transit
    // rising 8 ms above the best seen. Back off 25 %, then probe up 5 % per calm second.
    void onStats(const kp::ClientStats& s) {
        const uint32_t net = s.networkUsAvg;
        // Compare with frames actually sent: a slow SteamVR frame rate is not network congestion.
        const uint64_t enc = encoded_.load();
        const uint32_t sentDelta = static_cast<uint32_t>(enc - lastEncoded_);
        lastEncoded_ = enc;
        // Baseline = lowest per-second transit seen (0 = no frame in that report: ignored).
        if (net && (bestNetUs_ == 0 || net < bestNetUs_)) bestNetUs_ = net;
        if (bestNetUs_) bestNetUs_ += 50;  // slowly forget an old optimum (route/channel changes)
        // Wi-Fi has isolated latency spikes unrelated to our rate: queueing delay must last 2 s.
        delayStrikes_ = (net && bestNetUs_ && net > bestNetUs_ + 10000) ? delayStrikes_ + 1 : 0;
        const bool congested = s.framesDropped > 0 || s.framesReceived + 5 < sentDelta ||
                               delayStrikes_ >= 2;
        uint32_t br = bitrateKbps_;
        if (++reports_ <= 5) {
            // SteamVR start-up (shader compiles, dashboard loading) causes transient stalls: ignore.
        } else if (congested) {
            const bool loss = s.framesDropped > 0 || s.framesReceived + 5 < sentDelta;
            const uint32_t floor = loss ? kMinKbps : std::min(kDelayFloorKbps, br);
            br = std::max<uint32_t>(floor, br * 3 / 4);
            calm_ = -2;  // hold 2 s before probing again
        } else if (++calm_ >= 1) {
            br = std::min<uint32_t>(cfg_.bitrateKbps, br + br / 10);
        }
        if (br != bitrateKbps_) {
            bitrateKbps_ = br;
            pendingBitrate_ = br;
        }
        writeStats(s, br);
        klog::info("client: {} fps, {} lost, decode {:.1f} ms, transit {:.1f} ms (best {:.1f}), rtt {:.1f} ms -> {} Mbit/s{}",
                   s.framesReceived, s.framesDropped, s.decodeUsAvg / 1000.0, net / 1000.0, bestNetUs_ / 1000.0,
                   s.rttUs / 1000.0, br / 1000, congested ? " (congestion)" : "");
    }

    // IVRDisplayComponent -----------------------------------------------------
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override {
        *x = 0;
        *y = 0;
        *w = cfg_.width;
        *h = cfg_.height;
    }
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        *w = hello_.eyeWidth;
        *h = hello_.eyeHeight;
    }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) override {
        *x = eye == vr::Eye_Left ? 0 : cfg_.width / 2;
        *y = 0;
        *w = cfg_.width / 2;
        *h = cfg_.height;
    }
    void GetProjectionRaw(vr::EVREye eye, float* l, float* r, float* t, float* b) override {
        const auto& f = hello_.fov[eye == vr::Eye_Left ? 0 : 1];
        *l = std::tan(f.left);
        *r = std::tan(f.right);
        *t = -std::tan(f.up);  // OpenVR: +y is down in projection-raw space
        *b = -std::tan(f.down);
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        vr::DistortionCoordinates_t d{};  // lens distortion is handled by the Quest compositor
        d.rfRed[0] = d.rfGreen[0] = d.rfBlue[0] = u;
        d.rfRed[1] = d.rfGreen[1] = d.rfBlue[1] = v;
        return d;
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t* out, vr::EVREye, uint32_t, float u, float v) override {
        out->v[0] = u;
        out->v[1] = v;
        return true;
    }

    // IVRDriverDirectModeComponent ---------------------------------------------
    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* d, SwapTextureSet_t* out) override {
        uint64_t h[3] = {};
        if (gpu_.createSet(pid, d->nWidth, d->nHeight, d->nFormat, d->nSampleCount, h))
            for (int i = 0; i < 3; ++i) out->rSharedTextureHandles[i] = h[i];
        out->unTextureFlags = 0;
    }
    void DestroySwapTextureSet(vr::SharedTextureHandle_t h) override { gpu_.destroySet(h); }
    void DestroyAllSwapTextureSets(uint32_t pid) override { gpu_.destroyAllForPid(pid); }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t h[2], uint32_t (*idx)[2]) override {
        (*idx)[0] = gpu_.nextIndex(h[0]);
        (*idx)[1] = gpu_.nextIndex(h[1]);
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&eyes)[2]) override {
        std::array<kage::driver::LayerEye, 2> l;
        for (int i = 0; i < 2; ++i)
            l[i] = {eyes[i].hTexture, eyes[i].bounds.uMin, eyes[i].bounds.vMin, eyes[i].bounds.uMax, eyes[i].bounds.vMax};
        if (layers_.empty()) renderPose_ = eyes[0].mHmdPose;  // first layer = scene, carries the render pose
        layers_.push_back(l);
    }
    void Present(vr::SharedTextureHandle_t sync) override {
        const uint64_t n = ++presents_;
        if (n <= 3) klog::info("Present #{}: {} layer(s), sync 0x{:x}", n, layers_.size(), sync);
        if (layers_.empty()) return;
        kp::VideoFrameMeta meta;
        {
            std::lock_guard l(poseMutex_);
            meta.trackingTimeUs = lastTrackingUs_;
        }
        meta.renderPose.position = {renderPose_.m[0][3], renderPose_.m[1][3], renderPose_.m[2][3]};
        meta.renderPose.orientation = quatFromMatrix(renderPose_);
        ID3D11Texture2D* tex = net_.clientActive() ? gpu_.compose(layers_, sync) : nullptr;
        layers_.clear();
        if (n <= 3) klog::info("Present #{}: composed={}", n, tex != nullptr);
        if (!tex) return;
        ++composed_;
        if (const uint32_t br = pendingBitrate_.exchange(0)) {  // applied on the encode thread
            encoder_.setBitrate(br);
            net_.setPacingKbps(br);
        }
        const uint64_t index = frameIndex_++;
        {
            std::lock_guard l(metaMutex_);
            metas_[index % kMetaRing] = meta;
        }
        const bool ok = encoder_.submit(tex, index, idr_.exchange(false) || index == 0);
        if (n <= 3) klog::info("Present #{}: submitted={}", n, ok);
        if (ok) ++submitted_;
    }
    void PostPresent(const Throttling_t*) override {}

private:
    // Live metrics for the Windows GUI (%LOCALAPPDATA%\KageVR\stats.json, rewritten each second).
    void writeStats(const kp::ClientStats& s, uint32_t br) {
        wchar_t local[MAX_PATH];
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
        const std::wstring dir = std::wstring(local) + L"\\KageVR\\";
        const std::string json = std::format(
            "{{\"timeMs\":{},\"fps\":{},\"lost\":{},\"decodeMs\":{:.2f},\"transitMs\":{:.2f},\"rttMs\":{:.2f},"
            "\"encodeMs\":{:.2f},\"bitrateMbps\":{:.1f},\"codec\":\"{}\",\"width\":{},\"height\":{},\"refreshHz\":{:.0f},"
            "\"predictionMs\":null}}",
            GetTickCount64(), s.framesReceived, s.framesDropped, s.decodeUsAvg / 1000.0, s.networkUsAvg / 1000.0,
            s.rttUs / 1000.0, encodeUsAvg_.load() / 1000.0, br / 1000.0, cfg_.codec == kp::kCodecHEVC ? "HEVC" : "H.264",
            cfg_.width, cfg_.height, cfg_.refreshHz);
        if (FILE* f = _wfopen((dir + L"stats.tmp").c_str(), L"wb")) {
            fwrite(json.data(), 1, json.size(), f);
            fclose(f);
            MoveFileExW((dir + L"stats.tmp").c_str(), (dir + L"stats.json").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    std::atomic<uint32_t> encodeUsAvg_{0};

    static constexpr size_t kMetaRing = 16;
    static constexpr uint32_t kMinKbps = 10'000, kStartKbps = 60'000;
    // Delay alone (no lost frame) never pushes below this: on a jittery Wi-Fi the delay
    // stays high even at low rates, and cutting further only degrades the image.
    static constexpr uint32_t kDelayFloorKbps = 50'000;
    uint32_t bitrateKbps_ = kStartKbps, bestNetUs_ = 0, delayStrikes_ = 0;
    uint64_t lastEncoded_ = 0;  // network thread only
    int calm_ = 0, reports_ = 0;
    std::atomic<uint32_t> pendingBitrate_{0};

    void vsyncLoop() {
        // High-resolution waitable timer: ~0.5 ms accuracy instead of the 15.6 ms default tick.
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const double period = 1.0 / cfg_.refreshHz;
        uint64_t next = kage::monotonicMicros(), lastWatch = next;
        while (running_) {
            next += static_cast<uint64_t>(period * 1e6);
            const int64_t wait = static_cast<int64_t>(next) - static_cast<int64_t>(kage::monotonicMicros());
            if (wait > 0 && timer) {
                LARGE_INTEGER due;
                due.QuadPart = -wait * 10;  // 100 ns units, relative
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            } else if (wait < -100000) {
                next = kage::monotonicMicros();  // fell far behind (debugger, sleep): resync
            }
            vr::VRServerDriverHost()->VsyncEvent(0);
            if (kage::monotonicMicros() - lastWatch > 2'000'000) {
                lastWatch = kage::monotonicMicros();
                klog::info("watch: presents {} composed {} submitted {} encoded {} tracking {} clientActive {}",
                           presents_.load(), composed_.load(), submitted_.load(), encoded_.load(), trackingCount_.load(),
                           net_.clientActive());
            }
        }
        if (timer) CloseHandle(timer);
    }

    void encoderLoop() {
        uint64_t frames = 0, bytes = 0, encUsSum = 0;
        uint64_t lastLog = kage::monotonicMicros();
        while (running_) {
            auto f = encoder_.poll();
            if (!f) continue;
            const uint64_t now = kage::monotonicMicros();
            kp::VideoFrameMeta meta;
            {
                std::lock_guard l(metaMutex_);
                meta = metas_[f->frameIndex % kMetaRing];
            }
            net_.sendVideo(static_cast<uint32_t>(f->frameIndex), f->keyframe, meta, f->data);
            ++encoded_;
            ++frames;
            bytes += f->data.size();
            encUsSum += now - f->submitUs;
            if (now - lastLog > 5'000'000) {
                const double secs = (now - lastLog) / 1e6;
                if (frames) encodeUsAvg_ = static_cast<uint32_t>(encUsSum / frames);
                klog::info("video: {:.1f} fps, {:.1f} Mbit/s, encode {:.2f} ms avg", frames / secs, bytes * 8 / secs / 1e6,
                           frames ? encUsSum / 1000.0 / frames : 0.0);
                frames = bytes = encUsSum = 0;
                lastLog = now;
            }
        }
    }

    kage::driver::StreamServer& net_;
    kp::StreamHello hello_;
    kp::StreamConfig cfg_;
    kage::driver::D3DCompositor gpu_;
    kage::driver::AmfEncoder encoder_;
    uint32_t id_ = vr::k_unTrackedDeviceIndexInvalid;
    std::mutex poseMutex_;
    vr::DriverPose_t pose_ = makePose({}, false, true);
    uint64_t lastTrackingUs_ = 0;
    std::vector<std::array<kage::driver::LayerEye, 2>> layers_;
    vr::HmdMatrix34_t renderPose_{};
    std::atomic<bool> idr_{true};
    uint64_t frameIndex_ = 0;
    std::atomic<uint64_t> presents_{0}, composed_{0}, submitted_{0}, encoded_{0}, trackingCount_{0};
    std::mutex metaMutex_;
    std::array<kp::VideoFrameMeta, kMetaRing> metas_{};
    std::atomic<bool> running_{false};
    std::thread vsync_, encodeOut_;
};

// ---------------------------------------------------------------------------
class Provider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        std::filesystem::path dir = L".";
        wchar_t* local = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&local, &len, L"LOCALAPPDATA") == 0 && local) {
            dir = std::filesystem::path(local) / L"KageVR";
            free(local);
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            klog::setFile((dir / L"driver.log").string());
        }
        klog::info("KAGE driver loaded");
        dataDir_ = dir;
        server_.onTracking = [this](const kp::TrackingState& t) {
            if (auto* h = hmd_.load()) h->update(t);
#ifdef KAGE_WITH_RIFT
            rift_.setReference(t.controllers[1], (static_cast<double>(t.predictedDisplayTimeUs) - static_cast<double>(t.sampleTimeUs)) / 1e6);
#endif
            for (auto& p : ctrlPtr_)
                if (auto* c = p.load()) c->update(t.controllers[static_cast<int>(c->hand())]);
        };
        server_.onIdrRequest = [this] {
            if (auto* h = hmd_.load()) h->requestIdr();
        };
        server_.onCalibrateFullBody = [this] {  // same trigger as the GUI button
            std::ofstream(dataDir_ / L"calibrate.request") << 1;
            klog::info("full body calibration requested from the headset");
        };
        server_.onStats = [this](const kp::ClientStats& s) {
            if (auto* h = hmd_.load()) h->onStats(s);
        };
        if (!server_.start(kp::kDefaultStreamPort, dir)) return vr::VRInitError_Driver_Failed;
        audio_.enabled = [this] { return server_.clientActive(); };
        audio_.onChunk = [this](uint32_t rate, std::span<const int16_t> pcm) { server_.sendAudio(rate, pcm); };
        audio_.start();
#ifdef KAGE_WITH_RIFT
        std::error_code fbEc;
        fullBody_ = std::filesystem::exists(dir / L"fullbody.enabled", fbEc);  // started from RunFrame (retries)
#endif
        return vr::VRInitError_None;
    }
    void Cleanup() override {
        audio_.stop();
#ifdef KAGE_WITH_RIFT
        rift_.stop();
#endif
        server_.stop();
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

    void RunFrame() override {
        if (const uint64_t now = GetTickCount64(); now - lastOffsetLoad_ > 2000) {  // live tuning
            lastOffsetLoad_ = now;
            reloadControllerOffset(dataDir_ / L"controller_offset.txt");
#ifdef KAGE_WITH_RIFT
            // Retry every few seconds: Meta Horizon Link may start after SteamVR.
            if (fullBody_ && !rift_.started() && now - lastRiftTry_ > 5000) {
                lastRiftTry_ = now;
                rift_.start(dataDir_);
            }
#endif
        }
        // Devices are added once the headset said hello: its display geometry defines the HMD.
        if (!hmdObj_) {
            auto hello = server_.hello();
            auto cfg = server_.config();
            if (hello && cfg) {
                hmdObj_ = std::make_unique<Hmd>(server_, *hello, *cfg);
                if (!hmdObj_->initGraphics()) {
                    klog::error("graphics/encoder initialisation failed, HMD not added");
                    return;
                }
                vr::VRServerDriverHost()->TrackedDeviceAdded("KAGE-HMD", vr::TrackedDeviceClass_HMD, hmdObj_.get());
                hmd_ = hmdObj_.get();
                for (int i = 0; i < 2; ++i) {
                    ctrl_[i] = std::make_unique<Controller>(static_cast<kp::Hand>(i));
                    vr::VRServerDriverHost()->TrackedDeviceAdded(ctrl_[i]->serial(), vr::TrackedDeviceClass_Controller, ctrl_[i].get());
                    ctrlPtr_[i] = ctrl_[i].get();
                }
            }
        }
        vr::VREvent_t ev;
        while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof ev)) {
            if (ev.eventType != vr::VREvent_Input_HapticVibration) continue;
            const auto& hv = ev.data.hapticVibration;
            for (auto& c : ctrl_) {
                if (!c || c->container() != hv.containerHandle || c->haptic() != hv.componentHandle) continue;
                server_.sendHaptics({c->hand(), static_cast<uint32_t>(hv.fDurationSeconds * 1e6f), hv.fFrequency,
                                     std::clamp(hv.fAmplitude, 0.f, 1.f)});
            }
        }
    }

private:
    kage::driver::StreamServer server_;
    kage::driver::AudioCapture audio_;
#ifdef KAGE_WITH_RIFT
    kage::driver::RiftTracking rift_;
    bool fullBody_ = false;
    uint64_t lastRiftTry_ = 0;
#endif
    std::filesystem::path dataDir_;
    uint64_t lastOffsetLoad_ = 0;
    std::unique_ptr<Hmd> hmdObj_;
    std::atomic<Hmd*> hmd_{nullptr};
    std::unique_ptr<Controller> ctrl_[2];
    std::atomic<Controller*> ctrlPtr_[2] = {nullptr, nullptr};  // read by the network thread
};

Provider g_provider;

}  // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name, int* code) {
    if (!std::strcmp(name, vr::IServerTrackedDeviceProvider_Version)) return &g_provider;
    if (code) *code = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
