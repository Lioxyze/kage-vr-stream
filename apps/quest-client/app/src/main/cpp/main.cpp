// KAGE VR Stream - Quest client entry point (Phase 1).
//
// - OpenXR session rendering a solid colour per state:
//     dark violet  = searching for a host on the LAN
//     cyan         = host found, ping/pong exchanges succeeding
//     orange       = host found but no longer answering (reconnecting)
// - Background network thread: UDP broadcast discovery, then Ping/Pong every
//   500 ms with clock-offset estimation. Results go to logcat (tag KageVR):
//     adb logcat -s KageVR
#include <android_native_app_glue.h>
#include <sys/system_properties.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "kage/protocol/messages.hpp"
#include "kage/session/client_link.hpp"
#include "kage/telemetry/stats.hpp"
#include "kage/util/log.hpp"
#include "xr_app.hpp"

namespace klog = kage::log;

namespace {

enum class NetState : int { Searching = 0, Connected = 1, Lost = 2 };

class NetworkClient {
public:
    void start() { thread_ = std::thread(&NetworkClient::run, this); }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    NetState state() const { return state_.load(); }

private:
    static std::string deviceName() {
        char model[PROP_VALUE_MAX] = {};
        return __system_property_get("ro.product.model", model) > 0 ? std::string(model) : std::string("Quest");
    }

    void run() {
        kage::session::ClientLink link;
        if (!link.open()) {
            klog::error("network: cannot open UDP socket");
            return;
        }
        const auto name = deviceName();
        const auto bcast = kage::net::Endpoint::broadcast(kage::protocol::kDefaultDiscoveryPort);
        std::optional<kage::session::DiscoveredHost> host;
        kage::telemetry::RollingStats rtt(64);
        int failures = 0;
        auto lastReport = std::chrono::steady_clock::now();
        auto lastBurst = lastReport;

        while (!stop_) {
            if (!host) {
                state_ = NetState::Searching;
                host = link.discover(bcast, name, 1000);
                if (host) {
                    klog::info("host found: '{}' at {}", host->name, host->control.toString());
                    failures = 0;
                }
                continue;
            }
            if (auto s = link.ping(host->control, 300)) {
                rtt.add(static_cast<double>(s->rttUs));
                failures = 0;
                state_ = NetState::Connected;
            } else if (++failures >= 6) {  // ~3 s without answer: back to discovery
                klog::warn("host '{}' stopped answering, searching again", host->name);
                host.reset();
                state_ = NetState::Lost;
                continue;
            } else {
                state_ = NetState::Lost;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport > std::chrono::seconds(5) && !rtt.empty()) {
                klog::info("link idle (1 ping/500 ms): rtt median {:.2f} ms p99 {:.2f} ms, pongs {}/{}",
                           rtt.percentile(50) / 1000, rtt.percentile(99) / 1000, link.pongsReceived(), link.pingsSent());
                lastReport = now;
            }
            // Diagnostic burst: with continuous traffic the Wi-Fi radio stays awake, as it
            // will during streaming. Comparing both shows the cost of Wi-Fi power saving.
            if (now - lastBurst > std::chrono::seconds(10)) {
                kage::telemetry::RollingStats burst(100);
                int ok = 0;
                for (int i = 0; i < 100 && !stop_; ++i) {
                    if (auto s = link.ping(host->control, 100)) {
                        burst.add(static_cast<double>(s->rttUs));
                        ++ok;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!burst.empty())
                    klog::info("link burst (100 pings/5 ms): {}/100 ok, rtt min {:.2f} median {:.2f} p99 {:.2f} max {:.2f} ms",
                               ok, burst.min() / 1000, burst.percentile(50) / 1000, burst.percentile(99) / 1000,
                               burst.max() / 1000);
                lastBurst = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<NetState> state_{NetState::Searching};
};

std::array<float, 4> colorFor(NetState s) {
    switch (s) {
        case NetState::Connected: return {0.0f, 0.55f, 0.65f, 1.0f};  // cyan
        case NetState::Lost: return {0.65f, 0.35f, 0.0f, 1.0f};       // orange
        default: return {0.12f, 0.02f, 0.22f, 1.0f};                  // dark violet
    }
}

}  // namespace

void android_main(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    klog::info("KAGE VR Stream client 0.1.0 starting");

    XrApp xr;
    const bool xrReady = xr.init(app);
    if (!xrReady) {
        klog::error("OpenXR initialisation failed - see previous messages");
        ANativeActivity_finish(app->activity);
    }

    NetworkClient net;
    if (xrReady) net.start();

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
        if (xr.running()) xr.renderFrame(colorFor(net.state()));
    }

    net.stop();
    xr.shutdown();
    klog::info("KAGE VR Stream client stopped");
    app->activity->vm->DetachCurrentThread();
}
