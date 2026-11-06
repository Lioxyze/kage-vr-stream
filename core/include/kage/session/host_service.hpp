// KAGE VR Stream - host-side discovery responder and control-channel echo.
//
// Phase 1/2 scope: answers DiscoveryProbe with DiscoveryAnnounce and Ping
// with Pong (timestamped). It does NOT yet authenticate peers: pairing and
// encryption are Phase 2 and every other message type is ignored until then.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "kage/net/udp_socket.hpp"

namespace kage::session {

struct HostServiceConfig {
    std::string hostName = "KAGE Host";
    uint16_t discoveryPort = 47800;  // 0 = ephemeral (tests)
    uint16_t controlPort = 47801;    // 0 = ephemeral (tests)
    uint32_t bindIp = 0;             // 0 = all interfaces
};

struct HostServiceCounters {
    std::atomic<uint64_t> probesAnswered{0};
    std::atomic<uint64_t> pingsAnswered{0};
    std::atomic<uint64_t> packetsRejected{0};
};

class HostService {
public:
    explicit HostService(HostServiceConfig cfg) : cfg_(std::move(cfg)) {}
    ~HostService() { stop(); }

    bool start();  // false if a socket could not be bound (port in use, ...)
    void stop();

    uint16_t discoveryPort() const { return discoveryPort_; }
    uint16_t controlPort() const { return controlPort_; }
    const HostServiceCounters& counters() const { return counters_; }

    // Optional hook, called from the service threads (must be thread-safe).
    std::function<void(const std::string&)> onEvent;

private:
    void discoveryLoop();
    void controlLoop();

    HostServiceConfig cfg_;
    net::UdpSocket discovery_;
    net::UdpSocket control_;
    uint16_t discoveryPort_ = 0;
    uint16_t controlPort_ = 0;
    std::atomic<bool> running_{false};
    std::thread discoveryThread_;
    std::thread controlThread_;
    HostServiceCounters counters_;
};

}  // namespace kage::session
