#include "kage/session/client_link.hpp"

#include <array>
#include <random>
#include <vector>

#include "kage/protocol/messages.hpp"
#include "kage/protocol/packet.hpp"
#include "kage/util/time.hpp"

namespace kage::session {

using namespace kage::protocol;

namespace {
// Waits until `deadlineUs` for a datagram satisfying `accept`; ignores everything else.
template <class F>
bool receiveUntil(net::UdpSocket& s, uint64_t deadlineUs, F&& accept) {
    std::array<uint8_t, kMaxDatagram> buf{};
    for (;;) {
        const uint64_t now = monotonicMicros();
        if (now >= deadlineUs) return false;
        const int waitMs = static_cast<int>((deadlineUs - now + 999) / 1000);
        size_t n = 0;
        net::Endpoint from;
        const auto st = s.receive(buf, n, from, waitMs);
        if (st == net::RecvStatus::Timeout) return false;
        if (st != net::RecvStatus::Ok) continue;
        const uint64_t recvUs = monotonicMicros();
        PacketHeader h;
        std::span<const uint8_t> body;
        if (decodePacket({buf.data(), n}, h, body) != DecodeError::None) continue;
        auto msg = decodeMessage(body);
        if (msg && accept(h, *msg, from, recvUs)) return true;
    }
}
}  // namespace

bool ClientLink::open() {
    auto s = net::UdpSocket::open(0, 0, true);
    if (!s) return false;
    sock_ = std::move(*s);
    return true;
}

std::optional<DiscoveredHost> ClientLink::discover(const net::Endpoint& target, const std::string& clientName,
                                                   int timeoutMs) {
    if (!sock_.valid()) return std::nullopt;
    std::random_device rd;
    const uint64_t nonce = (static_cast<uint64_t>(rd()) << 32) | rd();

    std::vector<uint8_t> payload, out;
    encodeMessage(DiscoveryProbe{nonce, clientName}, payload);
    PacketHeader h;
    h.channel = Channel::Discovery;
    h.sequence = seq_++;
    h.sendTimeUs = monotonicMicros();
    encodePacket(h, payload, out);
    if (!sock_.sendTo(target, out)) return std::nullopt;

    std::optional<DiscoveredHost> found;
    receiveUntil(sock_, monotonicMicros() + static_cast<uint64_t>(timeoutMs) * 1000,
                 [&](const PacketHeader& rh, const Message& m, const net::Endpoint& from, uint64_t) {
                     const auto* a = std::get_if<DiscoveryAnnounce>(&m);
                     if (rh.channel != Channel::Discovery || !a || a->clientNonce != nonce) return false;
                     found = DiscoveredHost{a->hostName, {from.ipv4, a->controlPort}};
                     return true;
                 });
    return found;
}

std::optional<ConnectReply> ClientLink::requestConnect(const net::Endpoint& control, const std::string& clientName,
                                                      int timeoutMs) {
    if (!sock_.valid()) return std::nullopt;
    std::vector<uint8_t> payload, out;
    encodeMessage(ConnectRequest{clientName}, payload);
    PacketHeader h;
    h.channel = Channel::Control;
    h.sequence = seq_++;
    h.sendTimeUs = monotonicMicros();
    encodePacket(h, payload, out);
    if (!sock_.sendTo(control, out)) return std::nullopt;
    std::optional<ConnectReply> reply;
    receiveUntil(sock_, monotonicMicros() + static_cast<uint64_t>(timeoutMs) * 1000,
                 [&](const PacketHeader& rh, const Message& m, const net::Endpoint& from, uint64_t) {
                     const auto* r = std::get_if<ConnectReply>(&m);
                     if (rh.channel != Channel::Control || !r || !(from == control)) return false;
                     reply = *r;
                     return true;
                 });
    return reply;
}

std::optional<sync::ClockSample> ClientLink::ping(const net::Endpoint& control, int timeoutMs) {
    if (!sock_.valid()) return std::nullopt;
    const uint32_t id = nextPingId_++;
    const uint64_t t0 = monotonicMicros();

    std::vector<uint8_t> payload, out;
    encodeMessage(Ping{id, t0}, payload);
    PacketHeader h;
    h.channel = Channel::Control;
    h.sequence = seq_++;
    h.sendTimeUs = t0;
    encodePacket(h, payload, out);
    if (!sock_.sendTo(control, out)) return std::nullopt;
    ++pingsSent_;

    std::optional<sync::ClockSample> sample;
    receiveUntil(sock_, t0 + static_cast<uint64_t>(timeoutMs) * 1000,
                 [&](const PacketHeader& rh, const Message& m, const net::Endpoint& from, uint64_t t3) {
                     const auto* p = std::get_if<Pong>(&m);
                     if (rh.channel != Channel::Control || !p || p->pingId != id || p->clientSendUs != t0 ||
                         !(from == control))
                         return false;
                     sample = clock_.addSample(t0, p->hostRecvUs, p->hostSendUs, t3);
                     return sample.has_value();
                 });
    if (sample) ++pongsReceived_;
    return sample;
}

}  // namespace kage::session
