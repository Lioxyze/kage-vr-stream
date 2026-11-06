// KAGE VR Stream - Windows host (Phase 1: command-line host).
//
//   kage-host probe [--json]             hardware / runtime detection
//   kage-host serve [--name N]           answer discovery + ping (Ctrl+C to stop)
//   kage-host discover                   broadcast a probe on the LAN, list hosts
//   kage-host ping <ip> [--count N]      measure RTT / jitter / loss to a host
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "kage/protocol/messages.hpp"
#include "kage/session/client_link.hpp"
#include "kage/session/host_service.hpp"
#include "kage/telemetry/stats.hpp"
#include "kage/util/log.hpp"
#include "probe.hpp"

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI onCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

void print(const std::string& s) { std::fwrite(s.data(), 1, s.size(), stdout); }

std::string computerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return "KAGE Host";
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), len, nullptr, nullptr);
    return s;
}

std::string argValue(const std::vector<std::string>& a, const std::string& key, const std::string& def) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == key) return a[i + 1];
    return def;
}

bool hasFlag(const std::vector<std::string>& a, const std::string& f) {
    for (const auto& x : a)
        if (x == f) return true;
    return false;
}

int usage() {
    print("KAGE VR Stream host 0.1.0 (phase 1)\n"
          "usage:\n"
          "  kage-host probe [--json]\n"
          "  kage-host serve [--name NAME] [--discovery-port P] [--control-port P]\n"
          "  kage-host discover\n"
          "  kage-host ping <ip> [--port P] [--count N] [--interval-ms MS]\n");
    return 2;
}

int cmdProbe(const std::vector<std::string>& a) {
    const auto r = kage::host::probeSystem();
    print(hasFlag(a, "--json") ? kage::host::toJson(r) : kage::host::toText(r));
    return 0;
}

int cmdServe(const std::vector<std::string>& a) {
    kage::session::HostServiceConfig cfg;
    cfg.hostName = argValue(a, "--name", computerName());
    cfg.discoveryPort = static_cast<uint16_t>(std::stoi(argValue(a, "--discovery-port", "47800")));
    cfg.controlPort = static_cast<uint16_t>(std::stoi(argValue(a, "--control-port", "47801")));
    kage::session::HostService svc(cfg);
    svc.onEvent = [](const std::string& e) { kage::log::info("{}", e); };
    if (!svc.start()) return 1;
    kage::log::info("host '{}' ready. Windows may ask to allow kage-host on private networks (UDP {}/{}). Ctrl+C to stop.",
                    cfg.hostName, svc.discoveryPort(), svc.controlPort());
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    svc.stop();
    kage::log::info("stopped. probes answered: {}, pings answered: {}, rejected packets: {}",
                    svc.counters().probesAnswered.load(), svc.counters().pingsAnswered.load(),
                    svc.counters().packetsRejected.load());
    return 0;
}

int cmdDiscover() {
    kage::session::ClientLink link;
    if (!link.open()) {
        kage::log::error("cannot open UDP socket");
        return 1;
    }
    auto h = link.discover(kage::net::Endpoint::broadcast(kage::protocol::kDefaultDiscoveryPort), computerName(), 1500);
    if (!h) {
        print("Aucun hôte KAGE n'a répondu sur le réseau local (1,5 s).\n");
        return 1;
    }
    print(std::format("Hôte trouvé : '{}' — contrôle {}\n", h->name, h->control.toString()));
    return 0;
}

int cmdPing(const std::vector<std::string>& a) {
    if (a.size() < 2) return usage();
    const auto port = static_cast<uint16_t>(std::stoi(argValue(a, "--port", "47801")));
    auto ep = kage::net::Endpoint::parse(a[1], port);
    if (!ep) {
        kage::log::error("invalid IPv4 address '{}'", a[1]);
        return 2;
    }
    const int count = std::stoi(argValue(a, "--count", "100"));
    const int intervalMs = std::stoi(argValue(a, "--interval-ms", "10"));

    kage::session::ClientLink link;
    if (!link.open()) return 1;
    kage::telemetry::RollingStats rtt(static_cast<size_t>(count));
    kage::telemetry::JitterEstimator jitter;
    for (int i = 0; i < count && !g_stop; ++i) {
        if (auto s = link.ping(*ep, 500)) {
            rtt.add(static_cast<double>(s->rttUs));
            jitter.add(0, s->rttUs);  // RTT variation (one-way jitter needs synced clocks)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
    const auto sent = link.pingsSent(), got = link.pongsReceived();
    print(std::format("{} pings vers {} : {} réponses, perte {:.2f} %\n", sent, ep->toString(), got,
                      sent ? 100.0 * static_cast<double>(sent - got) / static_cast<double>(sent) : 0.0));
    if (rtt.empty()) {
        print("RTT : Non disponible (aucune réponse)\n");
        return 1;
    }
    print(std::format("RTT  min {:.2f} ms | médiane {:.2f} ms | p99 {:.2f} ms | max {:.2f} ms\n", rtt.min() / 1000,
                      rtt.percentile(50) / 1000, rtt.percentile(99) / 1000, rtt.max() / 1000));
    print(std::format("Variation RTT (estimateur RFC 3550) : {:.2f} ms\n", jitter.jitterUs() / 1000));
    print(std::format("Décalage d'horloge estimé (hôte - local) : {} µs\n", link.clock().offsetUs()));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onCtrl, TRUE);
    std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) return usage();
    if (hasFlag(a, "--verbose")) kage::log::setLevel(kage::log::Level::Debug);
    try {
        if (a[0] == "probe") return cmdProbe(a);
        if (a[0] == "serve") return cmdServe(a);
        if (a[0] == "discover") return cmdDiscover();
        if (a[0] == "ping") return cmdPing(a);
    } catch (const std::exception& e) {
        kage::log::error("invalid arguments: {}", e.what());
        return 2;
    }
    return usage();
}
