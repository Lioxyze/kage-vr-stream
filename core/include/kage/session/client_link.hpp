// KAGE VR Stream - client-side discovery and latency probing.
// Used by the Quest client and by the host's `ping` diagnostic command.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "kage/net/udp_socket.hpp"
#include "kage/sync/clock_sync.hpp"

namespace kage::session {

struct DiscoveredHost {
    std::string name;
    net::Endpoint control;  // address the announce came from + announced control port
};

class ClientLink {
public:
    // Opens an ephemeral UDP socket with broadcast enabled.
    bool open();

    // Sends one probe to `target` (usually broadcast:47800) and waits for an answer.
    std::optional<DiscoveredHost> discover(const net::Endpoint& target, const std::string& clientName, int timeoutMs);

    // One Ping/Pong round trip. Updates clock() on success and returns the sample.
    std::optional<sync::ClockSample> ping(const net::Endpoint& control, int timeoutMs);

    const sync::ClockSync& clock() const { return clock_; }
    uint64_t pingsSent() const { return pingsSent_; }
    uint64_t pongsReceived() const { return pongsReceived_; }

private:
    net::UdpSocket sock_;
    sync::ClockSync clock_;
    uint32_t nextPingId_ = 1;
    uint32_t seq_ = 0;
    uint64_t pingsSent_ = 0;
    uint64_t pongsReceived_ = 0;
};

}  // namespace kage::session
