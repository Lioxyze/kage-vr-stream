#include "kage/protocol/messages.hpp"

#include <cmath>

#include "kage/protocol/wire.hpp"

namespace kage::protocol {
namespace {

using wire::Reader;
using wire::Writer;

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

bool finite(float v) { return std::isfinite(v); }

bool readFloatIn(Reader& r, float& v, float lo, float hi) {
    if (!r.f32(v)) return false;
    if (!finite(v) || v < lo || v > hi) return r.fail();
    return true;
}

// Positions beyond 1 km or velocities beyond 1 km/s are not plausible for a
// tracked VR device and indicate corruption or a hostile sender.
constexpr float kMaxPosition = 1000.f;
constexpr float kMaxVelocity = 1000.f;

void writePose(Writer& w, const Pose& p) {
    for (float v : p.position) w.f32(v);
    for (float v : p.orientation) w.f32(v);
    for (float v : p.linearVelocity) w.f32(v);
    for (float v : p.angularVelocity) w.f32(v);
}

bool readPose(Reader& r, Pose& p) {
    for (float& v : p.position) readFloatIn(r, v, -kMaxPosition, kMaxPosition);
    for (float& v : p.orientation) readFloatIn(r, v, -1.001f, 1.001f);
    for (float& v : p.linearVelocity) readFloatIn(r, v, -kMaxVelocity, kMaxVelocity);
    for (float& v : p.angularVelocity) readFloatIn(r, v, -kMaxVelocity, kMaxVelocity);
    if (!r.ok()) return false;
    const auto& q = p.orientation;
    const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (std::fabs(n2 - 1.f) > 0.01f) return r.fail();  // must be a unit quaternion
    return true;
}

void writeController(Writer& w, const ControllerState& c) {
    w.u8(static_cast<uint8_t>(c.hand));
    w.u8(c.flags);
    writePose(w, c.pose);
    w.u32(c.buttons);
    w.u32(c.touches);
    w.f32(c.trigger);
    w.f32(c.grip);
    w.f32(c.thumbstickX);
    w.f32(c.thumbstickY);
}

bool readController(Reader& r, ControllerState& c) {
    uint8_t hand = 0;
    r.u8(hand);
    if (hand > 1) return r.fail();
    c.hand = static_cast<Hand>(hand);
    r.u8(c.flags);
    if (c.flags & ~(kPoseValid | kVelocityValid | kConnected)) return r.fail();
    readPose(r, c.pose);
    r.u32(c.buttons);
    r.u32(c.touches);
    readFloatIn(r, c.trigger, 0.f, 1.f);
    readFloatIn(r, c.grip, 0.f, 1.f);
    readFloatIn(r, c.thumbstickX, -1.f, 1.f);
    readFloatIn(r, c.thumbstickY, -1.f, 1.f);
    return r.ok();
}

}  // namespace

MessageType typeOf(const Message& m) {
    return std::visit(Overloaded{
                          [](const DiscoveryProbe&) { return MessageType::DiscoveryProbe; },
                          [](const DiscoveryAnnounce&) { return MessageType::DiscoveryAnnounce; },
                          [](const Ping&) { return MessageType::Ping; },
                          [](const Pong&) { return MessageType::Pong; },
                          [](const Heartbeat&) { return MessageType::Heartbeat; },
                          [](const Disconnect&) { return MessageType::Disconnect; },
                          [](const TrackingState&) { return MessageType::TrackingState; },
                      },
                      m);
}

void encodeMessage(const Message& m, std::vector<uint8_t>& out) {
    out.clear();
    Writer w(out);
    w.u8(static_cast<uint8_t>(typeOf(m)));
    std::visit(Overloaded{
                   [&](const DiscoveryProbe& p) {
                       w.u64(p.clientNonce);
                       w.str(p.clientName.substr(0, kMaxNameLength));
                   },
                   [&](const DiscoveryAnnounce& a) {
                       w.u64(a.clientNonce);
                       w.str(a.hostName.substr(0, kMaxNameLength));
                       w.u16(a.controlPort);
                       w.bytes(a.hostKeyFingerprint);
                       w.u32(a.capabilities);
                   },
                   [&](const Ping& p) {
                       w.u32(p.pingId);
                       w.u64(p.clientSendUs);
                   },
                   [&](const Pong& p) {
                       w.u32(p.pingId);
                       w.u64(p.clientSendUs);
                       w.u64(p.hostRecvUs);
                       w.u64(p.hostSendUs);
                   },
                   [&](const Heartbeat& h) { w.u64(h.uptimeUs); },
                   [&](const Disconnect& d) { w.u8(static_cast<uint8_t>(d.reason)); },
                   [&](const TrackingState& t) {
                       w.u64(t.sampleTimeUs);
                       w.u64(t.predictedDisplayTimeUs);
                       w.u8(t.headFlags);
                       writePose(w, t.head);
                       for (const auto& c : t.controllers) writeController(w, c);
                   },
               },
               m);
}

std::optional<Message> decodeMessage(std::span<const uint8_t> payload) {
    Reader r(payload);
    uint8_t type = 0;
    if (!r.u8(type)) return std::nullopt;

    std::optional<Message> result;
    switch (static_cast<MessageType>(type)) {
        case MessageType::DiscoveryProbe: {
            DiscoveryProbe p;
            r.u64(p.clientNonce);
            r.str(p.clientName, kMaxNameLength);
            result = std::move(p);
            break;
        }
        case MessageType::DiscoveryAnnounce: {
            DiscoveryAnnounce a;
            r.u64(a.clientNonce);
            r.str(a.hostName, kMaxNameLength);
            r.u16(a.controlPort);
            std::span<const uint8_t> fp;
            if (r.bytes(fp, a.hostKeyFingerprint.size()))
                std::copy(fp.begin(), fp.end(), a.hostKeyFingerprint.begin());
            r.u32(a.capabilities);
            if (a.controlPort == 0) r.fail();
            result = std::move(a);
            break;
        }
        case MessageType::Ping: {
            Ping p;
            r.u32(p.pingId);
            r.u64(p.clientSendUs);
            result = p;
            break;
        }
        case MessageType::Pong: {
            Pong p;
            r.u32(p.pingId);
            r.u64(p.clientSendUs);
            r.u64(p.hostRecvUs);
            r.u64(p.hostSendUs);
            if (r.ok() && p.hostSendUs < p.hostRecvUs) r.fail();
            result = p;
            break;
        }
        case MessageType::Heartbeat: {
            Heartbeat h;
            r.u64(h.uptimeUs);
            result = h;
            break;
        }
        case MessageType::Disconnect: {
            uint8_t reason = 0;
            r.u8(reason);
            if (reason > static_cast<uint8_t>(DisconnectReason::ProtocolError)) r.fail();
            result = Disconnect{static_cast<DisconnectReason>(reason)};
            break;
        }
        case MessageType::TrackingState: {
            TrackingState t;
            r.u64(t.sampleTimeUs);
            r.u64(t.predictedDisplayTimeUs);
            r.u8(t.headFlags);
            if (t.headFlags & ~(kPoseValid | kVelocityValid)) r.fail();
            readPose(r, t.head);
            for (auto& c : t.controllers) readController(r, c);
            if (r.ok() && (t.controllers[0].hand != Hand::Left || t.controllers[1].hand != Hand::Right))
                r.fail();
            result = std::move(t);
            break;
        }
        default:
            return std::nullopt;
    }
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return result;
}

}  // namespace kage::protocol
