#include "kage/protocol/messages.hpp"

#include <algorithm>
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

template <size_t N>
bool readKey(Reader& r, std::array<uint8_t, N>& k) {
    std::span<const uint8_t> v;
    if (!r.bytes(v, N)) return false;
    std::copy(v.begin(), v.end(), k.begin());
    return true;
}

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
                          [](const ConnectRequest&) { return MessageType::ConnectRequest; },
                          [](const ConnectReply&) { return MessageType::ConnectReply; },
                          [](const StreamHello&) { return MessageType::StreamHello; },
                          [](const StreamConfig&) { return MessageType::StreamConfig; },
                          [](const IdrRequest&) { return MessageType::IdrRequest; },
                          [](const Haptics&) { return MessageType::Haptics; },
                          [](const ClientStats&) { return MessageType::ClientStats; },
                          [](const PairRequest&) { return MessageType::PairRequest; },
                          [](const PairReply&) { return MessageType::PairReply; },
                          [](const SecureHello&) { return MessageType::SecureHello; },
                          [](const SecureAccept&) { return MessageType::SecureAccept; },
                          [](const CalibrateFullBody&) { return MessageType::CalibrateFullBody; },
                          [](const StreamFeatures&) { return MessageType::StreamFeatures; },
                          [](const BandwidthProbe&) { return MessageType::BandwidthProbe; },
                          [](const ProbeResult&) { return MessageType::ProbeResult; },
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
                   [&](const ConnectRequest& c) {
                       w.str(c.clientName.substr(0, kMaxNameLength));
                       w.bytes(c.clientKey);
                   },
                   [&](const PairRequest& c) {
                       w.str(c.clientName.substr(0, kMaxNameLength));
                       w.bytes(c.clientKey);
                   },
                   [&](const PairReply& c) {
                       w.u8(static_cast<uint8_t>(c.status));
                       w.bytes(c.hostKey);
                   },
                   [&](const SecureHello& c) {
                       w.bytes(c.clientStatic);
                       w.bytes(c.clientEphemeral);
                   },
                   [&](const CalibrateFullBody&) {},
                   [&](const StreamFeatures& f) {
                       w.u8(f.flags);
                       w.u8(f.foveation);
                   },
                   [&](const BandwidthProbe& p) {
                       w.u32(p.probeId);
                       w.u16(p.index);
                       w.u16(p.count);
                       for (uint16_t i = 0; i < p.padding; ++i) w.u8(0);
                   },
                   [&](const ProbeResult& p) {
                       w.u32(p.probeId);
                       w.u16(p.received);
                       w.u32(p.bytes);
                       w.u32(p.spanUs);
                   },
                   [&](const SecureAccept& c) {
                       w.bytes(c.hostEphemeral);
                       w.bytes(c.confirm);
                   },
                   [&](const ConnectReply& c) {
                       w.u8(static_cast<uint8_t>(c.status));
                       w.u16(c.streamPort);
                   },
                   [&](const StreamHello& h) {
                       w.u32(h.eyeWidth);
                       w.u32(h.eyeHeight);
                       w.f32(h.refreshHz);
                       for (const auto& f : h.fov) {
                           w.f32(f.left);
                           w.f32(f.right);
                           w.f32(f.up);
                           w.f32(f.down);
                       }
                       w.f32(h.ipdMeters);
                       w.u8(h.codecs);
                       w.u32(h.maxBitrateKbps);
                   },
                   [&](const StreamConfig& c) {
                       w.u8(c.codec);
                       w.u32(c.width);
                       w.u32(c.height);
                       w.f32(c.refreshHz);
                       w.u32(c.bitrateKbps);
                   },
                   [&](const IdrRequest& r) { w.u32(r.lastFrameIndex); },
                   [&](const Haptics& h) {
                       w.u8(static_cast<uint8_t>(h.hand));
                       w.u32(h.durationUs);
                       w.f32(h.frequency);
                       w.f32(h.amplitude);
                   },
                   [&](const ClientStats& s) {
                       w.u32(s.framesReceived);
                       w.u32(s.framesDropped);
                       w.u32(s.decodeUsAvg);
                       w.u32(s.networkUsAvg);
                       w.u32(s.rttUs);
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
        case MessageType::ConnectRequest: {
            ConnectRequest c;
            r.str(c.clientName, kMaxNameLength);
            readKey(r, c.clientKey);
            result = std::move(c);
            break;
        }
        case MessageType::PairRequest: {
            PairRequest c;
            r.str(c.clientName, kMaxNameLength);
            readKey(r, c.clientKey);
            result = std::move(c);
            break;
        }
        case MessageType::PairReply: {
            PairReply c;
            uint8_t st = 0;
            r.u8(st);
            if (st > static_cast<uint8_t>(PairStatus::Accepted)) r.fail();
            c.status = static_cast<PairStatus>(st);
            readKey(r, c.hostKey);
            result = c;
            break;
        }
        case MessageType::SecureHello: {
            SecureHello c;
            readKey(r, c.clientStatic);
            readKey(r, c.clientEphemeral);
            result = c;
            break;
        }
        case MessageType::CalibrateFullBody:
            result = CalibrateFullBody{};
            break;
        case MessageType::StreamFeatures: {
            StreamFeatures f;
            r.u8(f.flags);  // unknown bits ignored: future features stay compatible
            r.u8(f.foveation);
            if (f.foveation > kMaxFoveation) f.foveation = kMaxFoveation;
            result = f;
            break;
        }
        case MessageType::BandwidthProbe: {
            BandwidthProbe p;
            r.u32(p.probeId);
            r.u16(p.index);
            r.u16(p.count);
            p.padding = static_cast<uint16_t>(std::min<size_t>(r.rest().size(), 0xFFFF));
            result = p;
            break;
        }
        case MessageType::ProbeResult: {
            ProbeResult p;
            r.u32(p.probeId);
            r.u16(p.received);
            r.u32(p.bytes);
            r.u32(p.spanUs);
            result = p;
            break;
        }
        case MessageType::SecureAccept: {
            SecureAccept c;
            readKey(r, c.hostEphemeral);
            readKey(r, c.confirm);
            result = c;
            break;
        }
        case MessageType::ConnectReply: {
            ConnectReply c;
            uint8_t st = 0;
            r.u8(st);
            if (st > static_cast<uint8_t>(ConnectStatus::NotPaired)) r.fail();
            c.status = static_cast<ConnectStatus>(st);
            r.u16(c.streamPort);
            if (c.streamPort == 0) r.fail();
            result = c;
            break;
        }
        case MessageType::StreamHello: {
            StreamHello h;
            r.u32(h.eyeWidth);
            r.u32(h.eyeHeight);
            readFloatIn(r, h.refreshHz, 30.f, 240.f);
            constexpr float kMaxAngle = 1.56f;  // < 90 degrees
            for (auto& f : h.fov) {
                readFloatIn(r, f.left, -kMaxAngle, 0.5f);
                readFloatIn(r, f.right, -0.5f, kMaxAngle);
                readFloatIn(r, f.up, -0.5f, kMaxAngle);
                readFloatIn(r, f.down, -kMaxAngle, 0.5f);
            }
            readFloatIn(r, h.ipdMeters, 0.04f, 0.09f);
            r.u8(h.codecs);
            r.u32(h.maxBitrateKbps);
            if (r.ok() && (h.eyeWidth < 256 || h.eyeWidth > 4096 || h.eyeHeight < 256 || h.eyeHeight > 4096 ||
                           (h.codecs & ~(kCodecH264 | kCodecHEVC)) || !h.codecs))
                r.fail();
            result = h;
            break;
        }
        case MessageType::StreamConfig: {
            StreamConfig c;
            r.u8(c.codec);
            r.u32(c.width);
            r.u32(c.height);
            readFloatIn(r, c.refreshHz, 30.f, 240.f);
            r.u32(c.bitrateKbps);
            if (r.ok() && ((c.codec != kCodecH264 && c.codec != kCodecHEVC) || c.width < 256 || c.width > 8192 ||
                           c.height < 256 || c.height > 4096))
                r.fail();
            result = c;
            break;
        }
        case MessageType::IdrRequest: {
            IdrRequest q;
            r.u32(q.lastFrameIndex);
            result = q;
            break;
        }
        case MessageType::Haptics: {
            Haptics h;
            uint8_t hand = 0;
            r.u8(hand);
            if (hand > 1) r.fail();
            h.hand = static_cast<Hand>(hand);
            r.u32(h.durationUs);
            readFloatIn(r, h.frequency, 0.f, 10000.f);
            readFloatIn(r, h.amplitude, 0.f, 1.f);
            result = h;
            break;
        }
        case MessageType::ClientStats: {
            ClientStats s;
            r.u32(s.framesReceived);
            r.u32(s.framesDropped);
            r.u32(s.decodeUsAvg);
            r.u32(s.networkUsAvg);
            r.u32(s.rttUs);
            result = s;
            break;
        }
        default:
            return std::nullopt;
    }
    if (!r.ok() || r.remaining() != 0) return std::nullopt;
    return result;
}

void encodeFrameMeta(const VideoFrameMeta& m, std::vector<uint8_t>& out) {
    Writer w(out);
    w.u64(m.trackingTimeUs);
    for (float v : m.renderPose.position) w.f32(v);
    for (float v : m.renderPose.orientation) w.f32(v);
}

bool decodeFrameMeta(std::span<const uint8_t> in, VideoFrameMeta& m) {
    if (in.size() < VideoFrameMeta::kSize) return false;
    Reader r(in.first(VideoFrameMeta::kSize));
    r.u64(m.trackingTimeUs);
    for (float& v : m.renderPose.position) readFloatIn(r, v, -kMaxPosition, kMaxPosition);
    for (float& v : m.renderPose.orientation) readFloatIn(r, v, -1.001f, 1.001f);
    return r.ok();
}

}  // namespace kage::protocol
