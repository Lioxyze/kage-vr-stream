#include "kage/session/host_service.hpp"

#include <array>
#include <format>
#include <vector>

#include "kage/protocol/messages.hpp"
#include "kage/protocol/packet.hpp"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace kage::session {

using namespace kage::protocol;

bool HostService::start() {
    if (running_) return true;
    auto d = net::UdpSocket::open(cfg_.discoveryPort, cfg_.bindIp, true);
    if (!d) {
        log::error("discovery: cannot bind UDP port {} ({})", cfg_.discoveryPort, net::UdpSocket::lastErrorString());
        return false;
    }
    auto c = net::UdpSocket::open(cfg_.controlPort, cfg_.bindIp, false);
    if (!c) {
        log::error("control: cannot bind UDP port {} ({})", cfg_.controlPort, net::UdpSocket::lastErrorString());
        return false;
    }
    discovery_ = std::move(*d);
    control_ = std::move(*c);
    discoveryPort_ = discovery_.localPort();
    controlPort_ = control_.localPort();
    running_ = true;
    discoveryThread_ = std::thread(&HostService::discoveryLoop, this);
    controlThread_ = std::thread(&HostService::controlLoop, this);
    log::info("host service listening: discovery udp/{} control udp/{}", discoveryPort_, controlPort_);
    return true;
}

void HostService::stop() {
    if (!running_.exchange(false)) return;
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (controlThread_.joinable()) controlThread_.join();
}

void HostService::discoveryLoop() {
    std::array<uint8_t, kMaxDatagram> buf{};
    std::vector<uint8_t> payload, out;
    uint32_t seq = 0;
    while (running_) {
        size_t n = 0;
        net::Endpoint from;
        if (discovery_.receive(buf, n, from, 100) != net::RecvStatus::Ok) continue;

        PacketHeader h;
        std::span<const uint8_t> body;
        if (decodePacket({buf.data(), n}, h, body) != DecodeError::None || h.channel != Channel::Discovery) {
            ++counters_.packetsRejected;
            continue;
        }
        auto msg = decodeMessage(body);
        const auto* probe = msg ? std::get_if<DiscoveryProbe>(&*msg) : nullptr;
        if (!probe) {
            ++counters_.packetsRejected;
            continue;
        }
        DiscoveryAnnounce a;
        a.clientNonce = probe->clientNonce;
        a.hostName = cfg_.hostName;
        a.controlPort = controlPort_;
        encodeMessage(a, payload);
        PacketHeader rh;
        rh.channel = Channel::Discovery;
        rh.sequence = seq++;
        rh.sendTimeUs = monotonicMicros();
        encodePacket(rh, payload, out);
        if (discovery_.sendTo(from, out)) {
            ++counters_.probesAnswered;
            if (onEvent) onEvent(std::format("discovery probe from {} ('{}')", from.toString(), probe->clientName));
        }
    }
}

void HostService::controlLoop() {
    std::array<uint8_t, kMaxDatagram> buf{};
    std::vector<uint8_t> payload, out;
    uint32_t seq = 0;
    while (running_) {
        size_t n = 0;
        net::Endpoint from;
        if (control_.receive(buf, n, from, 100) != net::RecvStatus::Ok) continue;
        const uint64_t recvUs = monotonicMicros();

        PacketHeader h;
        std::span<const uint8_t> body;
        if (decodePacket({buf.data(), n}, h, body) != DecodeError::None || h.channel != Channel::Control) {
            ++counters_.packetsRejected;
            continue;
        }
        auto msg = decodeMessage(body);
        const auto* ping = msg ? std::get_if<Ping>(&*msg) : nullptr;
        if (!ping) {
            ++counters_.packetsRejected;  // Phase 2: authenticated session messages
            continue;
        }
        Pong p;
        p.pingId = ping->pingId;
        p.clientSendUs = ping->clientSendUs;
        p.hostRecvUs = recvUs;
        p.hostSendUs = monotonicMicros();
        encodeMessage(p, payload);
        PacketHeader rh;
        rh.channel = Channel::Control;
        rh.sessionId = h.sessionId;
        rh.sequence = seq++;
        rh.sendTimeUs = p.hostSendUs;
        encodePacket(rh, payload, out);
        if (control_.sendTo(from, out)) ++counters_.pingsAnswered;
    }
}

}  // namespace kage::session
