// KAGE VR Stream - Quest client.
//  Lobby  : floating panel, PCs found on the LAN, refresh-rate choice, hands
//           (hand tracking) and controller rays; click = trigger or finger poke.
//  Stream : SteamVR frames decoded in hardware and submitted with the pose they
//           were rendered with (the Quest compositor reprojects to the current
//           head pose). Long press on the left Menu button opens the session menu.
#include <android_native_app_glue.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "kage/crypto/secure.hpp"
#include "kage/protocol/messages.hpp"
#include "kage/session/client_link.hpp"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"
#include "renderer.hpp"
#include "stream_client.hpp"
#include "video_decoder.hpp"
#include "xr_app.hpp"
#include "xr_math.hpp"

namespace klog = kage::log;
namespace kp = kage::protocol;
using kage::monotonicMicros;

namespace {

// WifiManager.WIFI_MODE_FULL_LOW_LATENCY (API 29): turns Wi-Fi power save off while
// the app is in front. Without it the radio sleeps between beacons and adds
// tens of ms of jitter. Returns a global ref to the lock (kept for the app life).
jobject acquireLowLatencyWifi(JNIEnv* env, jobject activity) {
    auto clear = [&] {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return true;
        }
        return false;
    };
    jclass ctx = env->GetObjectClass(activity);
    jmethodID getSys = env->GetMethodID(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject wm = env->CallObjectMethod(activity, getSys, env->NewStringUTF("wifi"));
    if (clear() || !wm) return nullptr;
    jmethodID create = env->GetMethodID(env->GetObjectClass(wm), "createWifiLock",
                                        "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    jobject lock = env->CallObjectMethod(wm, create, 4 /* WIFI_MODE_FULL_LOW_LATENCY */, env->NewStringUTF("kage-stream"));
    if (clear() || !lock) return nullptr;
    env->CallVoidMethod(lock, env->GetMethodID(env->GetObjectClass(lock), "acquire", "()V"));
    if (clear()) return nullptr;
    klog::info("Wi-Fi low-latency lock acquired");
    return env->NewGlobalRef(lock);
}

std::string deviceName() {
    char model[PROP_VALUE_MAX] = {};
    return __system_property_get("ro.product.model", model) > 0 ? std::string(model) : std::string("Quest");
}

// ---------------------------------------------------------------------------
// Background LAN discovery + connect requests (never blocks the render thread).
class Lan {
public:
    struct Host {
        std::string name;
        kage::net::Endpoint control;
        uint64_t lastSeenUs = 0;
        float rttMs = -1;
    };
    struct ConnectResult {
        std::optional<kp::ConnectReply> reply;
        kage::crypto::Key hostKey{};
        std::string error;
    };
    void start(const kage::crypto::KeyPair& id, std::filesystem::path hostsFile) {
        id_ = id;
        hostsFile_ = std::move(hostsFile);
        thread_ = std::thread(&Lan::run, this);
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    void cancel() { cancel_ = true; }
    std::optional<uint32_t> pairingCode() const {
        std::lock_guard l(m_);
        return code_;
    }
    std::vector<Host> hosts() const {
        std::lock_guard l(m_);
        std::vector<Host> v;
        for (auto& [ip, h] : hosts_)
            if (monotonicMicros() - h.lastSeenUs < 5'000'000) v.push_back(h);
        return v;
    }
    void requestConnect(const kage::net::Endpoint& ep) {
        std::lock_guard l(m_);
        connectTo_ = ep;
        reply_ = {};
        replyReady_ = false;
        cancel_ = false;
    }
    bool takeReply(ConnectResult& r) {
        std::lock_guard l(m_);
        if (!replyReady_) return false;
        r = reply_;
        replyReady_ = false;
        return true;
    }

private:
    void run() {
        kage::session::ClientLink link;
        if (!link.open()) return;
        const auto name = deviceName();
        uint64_t lastProbe = 0;
        while (!stop_) {
            std::optional<kage::net::Endpoint> target;
            {
                std::lock_guard l(m_);
                target = connectTo_;
                connectTo_.reset();
            }
            if (target) {
                auto r = pairAndConnect(link, *target, name);
                std::lock_guard l(m_);
                code_.reset();
                reply_ = r;
                replyReady_ = true;
                continue;
            }
            if (monotonicMicros() - lastProbe > 1'000'000) {
                lastProbe = monotonicMicros();
                if (auto h = link.discover(kage::net::Endpoint::broadcast(kp::kDefaultDiscoveryPort), name, 400)) {
                    auto s = link.ping(h->control, 200);
                    std::lock_guard l(m_);
                    auto& e = hosts_[h->control.ipv4];
                    e.name = h->name;
                    e.control = h->control;
                    e.lastSeenUs = monotonicMicros();
                    e.rttMs = s ? s->rttUs / 1000.f : -1;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    // Pairing then connect. A PC key is trusted only if it was paired before, or
    // if it is the key whose code the user saw here while confirming on the PC.
    ConnectResult pairAndConnect(kage::session::ClientLink& link, const kage::net::Endpoint& target, const std::string& name) {
        ConnectResult res;
        std::optional<kage::crypto::Key> shown;
        const uint64_t deadline = monotonicMicros() + 180'000'000;
        while (!stop_ && !cancel_) {
            auto pr = link.requestPair(target, name, id_.pub, 800);
            if (!pr) {
                res.error = "Le PC ne répond pas (KageVR lancé sur le PC ? pare-feu ?)";
                return res;
            }
            if (pr->status == kp::PairStatus::Accepted) {
                if (!kage::crypto::isTrusted(hostsFile_, pr->hostKey) && !(shown && *shown == pr->hostKey)) {
                    res.error = "Clé du PC inconnue : oublie ce casque dans KageVR sur le PC puis réessaie";
                    return res;
                }
                kage::crypto::addTrusted(hostsFile_, pr->hostKey, "PC");
                res.hostKey = pr->hostKey;
                res.reply = link.requestConnect(target, name, id_.pub, 1500);
                if (!res.reply) res.error = "Le PC ne répond pas";
                return res;
            }
            if (shown && *shown != pr->hostKey) {
                res.error = "La clé du PC a changé pendant l'appairage : annulé";
                return res;
            }
            shown = pr->hostKey;
            {
                std::lock_guard l(m_);
                code_ = kage::crypto::pairingCode(id_.pub, pr->hostKey);
            }
            if (monotonicMicros() > deadline) {
                res.error = "Appairage non confirmé sur le PC";
                return res;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        res.error = "Annulé";
        return res;
    }

    std::thread thread_;
    std::atomic<bool> stop_{false}, cancel_{false};
    mutable std::mutex m_;
    std::map<uint32_t, Host> hosts_;
    std::optional<kage::net::Endpoint> connectTo_;
    ConnectResult reply_;
    bool replyReady_ = false;
    std::optional<uint32_t> code_;
    kage::crypto::KeyPair id_;
    std::filesystem::path hostsFile_;
};

// ---------------------------------------------------------------------------
// Flat UI panel in world space: buttons are rectangles in panel coordinates (meters).
struct Button {
    float x, y, w, h;  // centre, size
    std::string label;
    int id;
    bool enabled = true;
    bool selected = false;
};

// Stream rate (what SteamVR renders and we encode) and the display rate used for it.
// Below the display rate, the headset's compositor reprojects the missing refreshes.
struct FpsMode {
    int fps;
    float displayHz;
};
constexpr FpsMode kModes[] = {{45, 90.f}, {60, 120.f}, {72, 72.f}, {90, 90.f}};

class Panel {
public:
    XrPosef pose{{0, 0, 0, 1}, {0, 1.4f, -1.2f}};
    float width = 0.9f, height = 0.62f;

    void placeInFront(const XrPosef& head, float dist) {
        XrVector3f fwd = xm::rotate(head.orientation, {0, 0, -1});
        fwd.y = 0;
        const float l = xm::length(fwd);
        fwd = l > 1e-3f ? xm::scale(fwd, 1 / l) : XrVector3f{0, 0, -1};
        pose.position = xm::add(head.position, xm::scale(fwd, dist));
        pose.position.y = head.position.y - 0.12f;
        const float yaw = std::atan2(-fwd.x, -fwd.z);
        pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    }
    xm::Mat4 model() const { return xm::fromPose(pose); }

    // Ray/plane intersection in panel coordinates.
    bool hit(const XrPosef& ray, float& u, float& v, float& dist) const {
        const XrPosef inv = xm::inverse(pose);
        const XrPosef local = xm::compose(inv, ray);
        const XrVector3f dir = xm::rotate(local.orientation, {0, 0, -1});
        if (std::fabs(dir.z) < 1e-4f) return false;
        const float t = -local.position.z / dir.z;
        if (t <= 0) return false;
        u = local.position.x + dir.x * t;
        v = local.position.y + dir.y * t;
        dist = t;
        return std::fabs(u) <= width / 2 && std::fabs(v) <= height / 2;
    }
    // Point (finger tip) to panel coordinates; depth = signed distance in front of the panel.
    void toLocal(const XrVector3f& p, float& u, float& v, float& depth) const {
        const XrPosef inv = xm::inverse(pose);
        const XrVector3f l = xm::add(xm::rotate(inv.orientation, p), inv.position);
        u = l.x;
        v = l.y;
        depth = l.z;
    }
};

const Button* buttonAt(const std::vector<Button>& b, float u, float v) {
    for (const auto& x : b)
        if (x.enabled && std::fabs(u - x.x) <= x.w / 2 && std::fabs(v - x.y) <= x.h / 2) return &x;
    return nullptr;
}

std::string ms(float v) { return v >= 0 ? std::format("{:.1f} ms", v) : std::string("Non disponible"); }

enum class Mode { Lobby, Connecting, Streaming };
enum ButtonId { kBtnHost0 = 100, kBtnRate0 = 200, kBtnDisconnect = 300, kBtnResume = 301, kBtnRecenter = 302 };

// ---------------------------------------------------------------------------
class App {
public:
    App(XrApp& xr, std::filesystem::path dataDir) : xr_(xr), dataDir_(std::move(dataDir)) {}

    void start() {
        renderer_.init();
        // App-private storage: this headset's identity key and the PCs it trusts.
        auto id = kage::crypto::loadOrCreateKeyPair(dataDir_ / "client.key");
        if (!id) {
            klog::error("cannot create the headset key in {}", dataDir_.string());
            status_ = "Erreur : clé du casque impossible à créer";
        } else {
            identity_ = *id;
        }
        lan_.start(identity_, dataDir_ / "hosts.txt");
        // Default 60 images/s on a 120 Hz display: twice the bits per image (sharper)
        // and the headset reprojects every other refresh, so head motion stays fluid.
        if (!setMode(60)) setMode(72);
    }
    void stop() {
        stream_.stop();
        decoder_.shutdown();
        lan_.stop();
    }

    void frame() {
        xr_.frame([this](FrameContext& c) { prepare(c); }, [this](int eye, const FrameContext& c) { draw(eye, c); });
    }

private:
    // ---- per-frame logic -------------------------------------------------------
    void prepare(FrameContext& c) {
        ctx_ = c;
        if (!panelPlaced_ && c.headValid) {
            panel_.placeInFront(c.head, 1.1f);
            panelPlaced_ = true;
        }
        handleHaptics();
        updateMenuToggle();
        // Development only: `adb shell setprop debug.kage.autoconnect 1` connects to the first PC found.
        if (mode_ == Mode::Lobby && !autoTried_) {
            char v[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.kage.autoconnect", v) > 0 && v[0] == '1') {
                if (auto h = lan_.hosts(); !h.empty()) {
                    autoTried_ = true;
                    klog::info("autoconnect to {}", h[0].name);
                    connectTo(h[0]);
                }
            } else {
                autoTried_ = true;
            }
        }
        if (mode_ == Mode::Connecting) updateConnecting();
        if (mode_ == Mode::Streaming) updateStreaming(c);
        buildButtons();
        updatePointers();
    }

    bool modeAvailable(const FpsMode& m) const {
        const auto& r = xr_.refreshRates();
        return std::find(r.begin(), r.end(), m.displayHz) != r.end();
    }
    bool setMode(int fps) {
        for (const auto& m : kModes)
            if (m.fps == fps && modeAvailable(m)) {
                streamFps_ = fps;
                xr_.setRefreshRate(m.displayHz);
                return true;
            }
        return false;
    }

    void updateMenuToggle() {
        const bool menu = xr_.controllers()[0].menu;
        const uint64_t now = monotonicMicros();
        if (menu && !menuWasDown_) menuDownAt_ = now;
        if (menu && menuDownAt_ && now - menuDownAt_ > 600'000 && mode_ == Mode::Streaming) {
            overlay_ = !overlay_;
            menuDownAt_ = 0;  // one toggle per press
            if (overlay_ && ctx_.headValid) panel_.placeInFront(ctx_.head, 0.9f);
        }
        if (!menu) menuDownAt_ = 0;
        menuWasDown_ = menu;
    }

    void connectTo(const Lan::Host& h) {
        target_ = h;
        lan_.requestConnect(h.control);
        mode_ = Mode::Connecting;
        connectStartUs_ = monotonicMicros();
        status_ = "Connexion au PC…";
    }

    void updateConnecting() {
        pairCode_ = lan_.pairingCode();
        if (pairCode_) status_ = "Vérifie que le PC affiche le même code, puis accepte sur le PC";
        Lan::ConnectResult res;
        if (lan_.takeReply(res)) {
            const auto& reply = res.reply;
            if (!reply) {
                status_ = res.error;
                mode_ = Mode::Lobby;
                return;
            }
            if (reply->status == kp::ConnectStatus::NotPaired) {
                status_ = "Ce casque n'est pas appairé avec ce PC";
                mode_ = Mode::Lobby;
                return;
            }
            if (reply->status == kp::ConnectStatus::SteamVrMissing || reply->status == kp::ConnectStatus::Failed) {
                status_ = reply->status == kp::ConnectStatus::SteamVrMissing ? "SteamVR introuvable sur le PC"
                                                                              : "Le PC n'a pas pu lancer SteamVR";
                mode_ = Mode::Lobby;
                return;
            }
            status_ = reply->status == kp::ConnectStatus::Launching ? "Démarrage de SteamVR…" : "SteamVR déjà lancé, connexion…";
            kp::StreamHello hello;
            hello.eyeWidth = xr_.eyeWidth();
            hello.eyeHeight = xr_.eyeHeight();
            hello.refreshHz = static_cast<float>(streamFps_);  // SteamVR renders at this rate
            std::array<XrFovf, 2> fov{};
            float ipd = 0.063f;
            if (xr_.eyeFovAndIpd(fov, ipd))
                for (int i = 0; i < 2; ++i) hello.fov[i] = {fov[i].angleLeft, fov[i].angleRight, fov[i].angleUp, fov[i].angleDown};
            hello.ipdMeters = std::clamp(ipd, 0.045f, 0.085f);
            hello.codecs = kp::kCodecHEVC | kp::kCodecH264;
            hello.maxBitrateKbps = 150'000;
            stream_.start({target_.control.ipv4, reply->streamPort}, hello, identity_, res.hostKey);
        }
        if (stream_.running()) {
            const auto cfg = stream_.config();
            if (cfg && !decoder_.ready()) {
                if (decoder_.init(cfg->codec == kp::kCodecHEVC, static_cast<int>(cfg->width), static_cast<int>(cfg->height))) {
                    stream_.attachDecoder(&decoder_);
                    uScale_ = static_cast<float>(xr_.eyeWidth() * 2) / static_cast<float>(cfg->width);
                    status_ = "Flux vidéo en attente…";
                } else {
                    status_ = "Décodeur vidéo indisponible";
                    disconnect();
                    return;
                }
            }
            if (cfg) sendTracking(ctx_);  // SteamVR needs a valid pose to render at full rate
            // Drain decoded images now: an ImageReader that is never read stalls the codec.
            if (decoder_.ready()) decoder_.latest(xr_.eglDisplay());
            if (stream_.videoFlowing() && decoder_.hasFrame()) {
                mode_ = Mode::Streaming;
                overlay_ = false;
                klog::info("streaming started");
            }
        }
        if (mode_ == Mode::Connecting && monotonicMicros() - connectStartUs_ > 90'000'000) {
            status_ = "Délai dépassé : SteamVR n'a pas démarré le flux";
            disconnect();
        }
    }

    void disconnect() {
        stream_.stop();
        decoder_.shutdown();
        mode_ = Mode::Lobby;
        overlay_ = false;
        if (ctx_.headValid) panel_.placeInFront(ctx_.head, 1.1f);
    }

    void sendTracking(const FrameContext& c) {
        const XrTime target = c.displayTime + static_cast<XrTime>(predictionUs_) * 1000;
        kp::TrackingState t;
        t.sampleTimeUs = static_cast<uint64_t>(xr_.now() / 1000);
        t.predictedDisplayTimeUs = static_cast<uint64_t>(target / 1000);
        XrPosef head;
        if (xr_.locateHead(target, head)) {
            t.headFlags = kp::kPoseValid;
            t.head.position = {head.position.x, head.position.y, head.position.z};
            t.head.orientation = {head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
        }
        const bool block = overlay_;  // the session menu captures input
        for (int i = 0; i < 2; ++i) {
            const auto& in = xr_.controllers()[i];
            auto& o = t.controllers[i];
            o.hand = static_cast<kp::Hand>(i);
            XrPosef p;
            if (in.active && xr_.locateController(i, target, p)) {
                o.flags = kp::kConnected | kp::kPoseValid;
                o.pose.position = {p.position.x, p.position.y, p.position.z};
                o.pose.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
            }
            if (block || !in.active) continue;
            o.trigger = std::clamp(in.trigger, 0.f, 1.f);
            o.grip = std::clamp(in.squeeze, 0.f, 1.f);
            o.thumbstickX = std::clamp(in.stickX, -1.f, 1.f);
            o.thumbstickY = std::clamp(in.stickY, -1.f, 1.f);
            const uint32_t b1 = i == 0 ? kp::kButtonX : kp::kButtonA, b2 = i == 0 ? kp::kButtonY : kp::kButtonB;
            o.buttons = (in.btn1 ? b1 : 0) | (in.btn2 ? b2 : 0) | (in.stickClick ? kp::kButtonThumbstick : 0) |
                        (in.menu && !menuDownAt_ ? kp::kButtonMenu : 0);
            o.touches = (in.btn1Touch ? b1 : 0) | (in.btn2Touch ? b2 : 0) | (in.stickTouch ? kp::kButtonThumbstick : 0) |
                        (in.triggerTouch ? kp::kButtonTrigger : 0) | (in.thumbrest ? kp::kButtonThumbrest : 0);
        }
        stream_.sendTracking(t);
    }

    void updateStreaming(FrameContext& c) {
        sendTracking(c);
        if (decoder_.latest(xr_.eglDisplay())) {
            frameMeta_ = stream_.meta(decoder_.frameIndex());
            // Self-calibrating prediction: the pose was predicted for trackingTimeUs, the frame
            // is shown at displayTime. Converge prediction towards the real pipeline latency.
            const double err = static_cast<double>(c.displayTime / 1000) - static_cast<double>(frameMeta_.trackingTimeUs);
            if (frameMeta_.trackingTimeUs && std::fabs(err) < 200'000)
                predictionUs_ = std::clamp(predictionUs_ + 0.05 * err, 5'000.0, 120'000.0);
        }
        if (!decoder_.hasFrame()) return;
        const auto& rp = frameMeta_.renderPose;
        const XrPosef render{{rp.orientation[0], rp.orientation[1], rp.orientation[2], rp.orientation[3]},
                             {rp.position[0], rp.position[1], rp.position[2]}};
        for (int i = 0; i < 2; ++i) c.submitPose[i] = xm::compose(render, c.eyeInHead[i]);
        ctx_ = c;
        if (!stream_.videoFlowing()) {
            if (!lostSinceUs_) lostSinceUs_ = monotonicMicros();
            if (monotonicMicros() - lostSinceUs_ > 10'000'000) {
                status_ = "Flux interrompu";
                disconnect();
            }
        } else {
            lostSinceUs_ = 0;
        }
    }

    void handleHaptics() {
        while (auto h = stream_.popHaptics())
            xr_.vibrate(static_cast<int>(h->hand), h->amplitude, h->frequency, static_cast<int64_t>(h->durationUs) * 1000);
    }

    void buildButtons() {
        buttons_.clear();
        const float top = panel_.height / 2;
        if (mode_ == Mode::Lobby || mode_ == Mode::Connecting) {
            hosts_ = lan_.hosts();
            for (size_t i = 0; i < hosts_.size() && i < 3 && !pairCode_; ++i)
                buttons_.push_back({0, top - 0.20f - i * 0.085f, 0.78f, 0.07f, "", kBtnHost0 + static_cast<int>(i),
                                    mode_ == Mode::Lobby});
            const float bw = 0.14f;
            if (mode_ == Mode::Connecting)
                buttons_.push_back({0.30f, -top + 0.10f, 0.22f, 0.06f, "Annuler", kBtnDisconnect});
            for (size_t i = 0; i < std::size(kModes) && mode_ == Mode::Lobby; ++i) {
                if (!modeAvailable(kModes[i])) continue;
                buttons_.push_back({-0.30f + i * (bw + 0.02f), -top + 0.10f, bw, 0.06f,
                                    std::format("{} i/s", kModes[i].fps), kBtnRate0 + static_cast<int>(i), true});
                buttons_.back().selected = kModes[i].fps == streamFps_;
            }
        } else if (overlay_) {
            buttons_.push_back({-0.26f, -top + 0.09f, 0.3f, 0.07f, "Reprendre", kBtnResume});
            buttons_.push_back({0.05f, -top + 0.09f, 0.22f, 0.07f, "Recentrer", kBtnRecenter});
            buttons_.push_back({0.30f, -top + 0.09f, 0.24f, 0.07f, "Déconnecter", kBtnDisconnect});
        }
    }

    void onClick(int id) {
        if (id >= kBtnHost0 && id < kBtnHost0 + 10) {
            const size_t i = static_cast<size_t>(id - kBtnHost0);
            if (i < hosts_.size()) connectTo(hosts_[i]);
        } else if (id >= kBtnRate0 && id < kBtnRate0 + 10) {
            const size_t i = static_cast<size_t>(id - kBtnRate0);
            if (i < std::size(kModes)) setMode(kModes[i].fps);
        } else if (id == kBtnDisconnect) {
            lan_.cancel();
            pairCode_.reset();
            status_.clear();
            disconnect();
        } else if (id == kBtnResume) {
            overlay_ = false;
        } else if (id == kBtnRecenter && ctx_.headValid) {
            panel_.placeInFront(ctx_.head, 0.9f);
        }
    }

    bool panelVisible() const { return mode_ != Mode::Streaming || overlay_; }

    void updatePointers() {
        hovered_ = -1;
        rays_.clear();
        if (!panelVisible()) return;
        for (int i = 0; i < 2; ++i) {
            const auto& c = xr_.controllers()[i];
            if (!c.active) continue;
            float u, v, d;
            const bool hit = panel_.hit(c.aim, u, v, d);
            const XrVector3f dir = xm::rotate(c.aim.orientation, {0, 0, -1});
            rays_.push_back(c.aim.position);
            rays_.push_back(xm::add(c.aim.position, xm::scale(dir, hit ? d : 2.f)));
            if (const Button* b = hit ? buttonAt(buttons_, u, v) : nullptr) {
                hovered_ = b->id;
                const bool pressed = c.trigger > 0.8f;
                if (pressed && !triggerWasDown_[i]) {
                    xr_.vibrate(i, 0.4f, 0, 20'000'000);
                    onClick(b->id);
                }
            }
            triggerWasDown_[i] = c.trigger > 0.8f;
        }
        // Finger poke with hand tracking: index tip crossing the panel plane over a button.
        for (int i = 0; i < 2; ++i) {
            const auto& h = xr_.hands()[i];
            if (!h.active) {
                pokeDown_[i] = false;
                continue;
            }
            float u, v, depth;
            panel_.toLocal(h.joints[XR_HAND_JOINT_INDEX_TIP_EXT].position, u, v, depth);
            const Button* b = buttonAt(buttons_, u, v);
            if (b && depth < 0.06f) hovered_ = b->id;
            const bool down = depth < 0.01f && depth > -0.05f;
            if (down && !pokeDown_[i] && b) onClick(b->id);
            pokeDown_[i] = down;
        }
    }

    // ---- drawing ------------------------------------------------------------------
    void draw(int eye, const FrameContext& c) {
        XrView v = c.views[eye];
        v.pose = c.submitPose[eye];
        renderer_.setViewProj(xm::viewProj(v));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        if (mode_ == Mode::Streaming && decoder_.hasFrame()) {
            glDisable(GL_BLEND);
            renderer_.video(decoder_.texture(), eye, uScale_);
        } else {
            glClearColor(0.004f, 0.002f, 0.012f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            drawEnvironment();
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (panelVisible()) drawPanel();
        if (mode_ != Mode::Streaming || overlay_) drawHandsAndRays();
    }

    void drawEnvironment() {
        // Floor grid at y = 0 (STAGE origin), 8 m wide.
        std::vector<XrVector3f> grid;
        for (int i = -8; i <= 8; ++i) {
            const float k = i * 0.5f;
            grid.push_back({k, 0, -4});
            grid.push_back({k, 0, 4});
            grid.push_back({-4, 0, k});
            grid.push_back({4, 0, k});
        }
        renderer_.lines(grid, {0.25f, 0.12f, 0.55f, 1});
    }

    void drawHandsAndRays() {
        for (const auto& h : xr_.hands()) {
            if (!h.active) continue;
            for (int k = 0; k < XR_HAND_JOINT_COUNT_EXT; ++k) {
                const float s = std::max(0.006f, h.radii[k] * 1.6f);
                renderer_.cube(xm::fromPose(h.joints[k], s), k == XR_HAND_JOINT_INDEX_TIP_EXT ? palette::kCyan : palette::kViolet);
            }
        }
        for (int i = 0; i < 2; ++i) {
            const auto& c = xr_.controllers()[i];
            if (!c.active) continue;
            renderer_.cube(xm::fromPoseScale(c.grip, 0.035f, 0.035f, 0.09f), {0.18f, 0.18f, 0.22f, 1});
        }
        renderer_.lines(rays_, palette::kCyan);
    }

    void drawPanel() {
        const auto m = panel_.model();
        const float W = panel_.width, H = panel_.height, top = H / 2, left = -W / 2 + 0.05f;
        renderer_.quad(xm::mul(m, xm::fromPoseScale({{0, 0, 0, 1}, {0, 0, -0.002f}}, W, H, 1)), palette::kPanel);
        renderer_.quad(xm::mul(m, xm::fromPoseScale({{0, 0, 0, 1}, {0, top - 0.004f, -0.001f}}, W, 0.008f, 1)), palette::kViolet);
        renderer_.text(m, left, top - 0.075f, 0.035f, "KAGÉ VR Stream", palette::kText);

        for (const auto& b : buttons_) {
            const bool hov = b.id == hovered_;
            const Color bg = b.selected  ? Color{0.45f, 0.25f, 0.85f, 1}
                             : !b.enabled ? Color{0.10f, 0.10f, 0.12f, 0.9f}
                             : hov      ? Color{0.30f, 0.16f, 0.62f, 1}
                                        : Color{0.14f, 0.12f, 0.22f, 1};
            renderer_.quad(xm::mul(m, xm::fromPoseScale({{0, 0, 0, 1}, {b.x, b.y, 0}}, b.w, b.h, 1)), bg);
            if (!b.label.empty()) {
                const float tw = renderer_.textWidth(b.label, 0.022f);
                renderer_.text(m, b.x - tw / 2, b.y - 0.011f, 0.022f, b.label, palette::kText);
            }
        }

        if (mode_ == Mode::Streaming) {
            const auto s = stream_.stats();
            float y = top - 0.15f;
            auto line = [&](const std::string& t, Color c = palette::kText) {
                renderer_.text(m, left, y, 0.022f, t, c);
                y -= 0.045f;
            };
            line(std::format("Connecté à {}", target_.name), palette::kCyan);
            line(std::format("Images reçues : {:.0f} /s   Débit : {:.1f} Mbit/s", s.fps, s.mbps));
            line(std::format("Aller-retour réseau : {}   Transit : {}", ms(s.rttMs), ms(s.networkMs)));
            line(std::format("Décodage : {}   Images perdues : {}/s", ms(s.decodeMs), s.dropped));
            line(std::format("Latence de bout en bout (prédiction) : {:.1f} ms   {:.0f} Hz", predictionUs_ / 1000,
                             xr_.refreshRate()));
            return;
        }
        if (mode_ == Mode::Connecting && pairCode_) {
            renderer_.text(m, left, top - 0.15f, 0.024f, std::format("Appairage avec {}", target_.name), palette::kText);
            const auto code = std::format("{:03} {:03}", *pairCode_ / 1000, *pairCode_ % 1000);
            const float tw = renderer_.textWidth(code, 0.09f);
            renderer_.text(m, -tw / 2, -0.02f, 0.09f, code, palette::kCyan);
            if (!status_.empty()) renderer_.text(m, left, -top + 0.035f, 0.02f, status_, palette::kWarn);
            return;
        }
        renderer_.text(m, left, top - 0.13f, 0.022f, "Ordinateurs sur le réseau local", palette::kDim);
        if (hosts_.empty())
            renderer_.text(m, left, top - 0.21f, 0.024f, "Recherche… (lancer kage-host sur le PC)", palette::kDim);
        for (size_t i = 0; i < hosts_.size() && i < 3; ++i) {
            const auto& h = hosts_[i];
            const float y = top - 0.20f - i * 0.085f - 0.012f;
            renderer_.text(m, left + 0.02f, y, 0.025f, std::format("{}  ({})", h.name, h.control.toString()), palette::kText);
            renderer_.text(m, W / 2 - 0.20f, y, 0.022f, h.rttMs >= 0 ? std::format("{:.1f} ms", h.rttMs) : "", palette::kCyan);
        }
        renderer_.text(m, left, -top + 0.17f, 0.02f, "Images par seconde (moins = image plus nette)", palette::kDim);
        if (!status_.empty()) renderer_.text(m, left, -top + 0.035f, 0.02f, status_, palette::kWarn);
    }

    XrApp& xr_;
    Renderer renderer_;
    Lan lan_;
    StreamClient stream_;
    VideoDecoder decoder_;
    Panel panel_;
    bool panelPlaced_ = false;
    Mode mode_ = Mode::Lobby;
    bool overlay_ = false;
    std::vector<Lan::Host> hosts_;
    Lan::Host target_;
    std::vector<Button> buttons_;
    int hovered_ = -1;
    bool triggerWasDown_[2] = {}, pokeDown_[2] = {};
    std::vector<XrVector3f> rays_;
    std::string status_;
    std::filesystem::path dataDir_;
    kage::crypto::KeyPair identity_;
    std::optional<uint32_t> pairCode_;
    int streamFps_ = 60;
    uint64_t connectStartUs_ = 0, lostSinceUs_ = 0, menuDownAt_ = 0;
    bool menuWasDown_ = false, autoTried_ = false;
    FrameContext ctx_;
    kp::VideoFrameMeta frameMeta_;
    float uScale_ = 1.f;
    double predictionUs_ = 40'000;  // initial guess, self-calibrated while streaming
};

}  // namespace

void android_main(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    klog::info("KAGE VR Stream client 0.3.0 starting");
    jobject wifiLock = acquireLowLatencyWifi(env, app->activity->clazz);
    if (!wifiLock) klog::warn("Wi-Fi low-latency lock unavailable");

    XrApp xr;
    const bool xrReady = xr.init(app);
    if (!xrReady) {
        klog::error("OpenXR initialisation failed");
        ANativeActivity_finish(app->activity);
    }
    App ui(xr, app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp");
    if (xrReady) ui.start();

    bool finishing = !xrReady;
    while (!app->destroyRequested) {
        int timeout = (xrReady && xr.running()) ? 0 : 100;
        int events = 0;
        android_poll_source* src = nullptr;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(app, src);
            timeout = 0;
            if (app->destroyRequested) break;
        }
        if (!xrReady) continue;
        bool exitRequested = false;
        xr.pollEvents(exitRequested);
        if (exitRequested && !finishing) {
            finishing = true;
            ANativeActivity_finish(app->activity);
        }
        if (xr.running()) ui.frame();
    }
    if (xrReady) ui.stop();
    xr.shutdown();
    if (wifiLock) {
        env->CallVoidMethod(wifiLock, env->GetMethodID(env->GetObjectClass(wifiLock), "release", "()V"));
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteGlobalRef(wifiLock);
    }
    app->activity->vm->DetachCurrentThread();
}
