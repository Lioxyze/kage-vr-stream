#include "stream_server.hpp"

#include <algorithm>
#include <array>

#include "kage/protocol/packet.hpp"
#include "kage/protocol/video_fragment.hpp"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace kage::driver {

using namespace kage::protocol;

bool StreamServer::start(uint16_t port) {
    auto s = net::UdpSocket::open(port, 0, false);
    if (!s) {
        log::error("stream: cannot bind udp/{} ({})", port, net::UdpSocket::lastErrorString());
        return false;
    }
    sock_ = std::move(*s);
    // Large kernel buffers absorb one encoded frame burst without drops.
    sock_.setBufferSizes(8 * 1024 * 1024, 1024 * 1024);
    running_ = true;
    thread_ = std::thread(&StreamServer::recvLoop, this);
    log::info("stream server listening on udp/{}", port);
    return true;
}

void StreamServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

std::optional<StreamHello> StreamServer::hello() const {
    std::lock_guard l(mutex_);
    return hello_;
}

std::optional<StreamConfig> StreamServer::config() const {
    std::lock_guard l(mutex_);
    return config_;
}

bool StreamServer::clientActive() const {
    const uint64_t last = lastRecvUs_;
    return last && monotonicMicros() - last < 2'000'000;
}

StreamConfig StreamServer::makeConfig(const StreamHello& h) const {
    StreamConfig c;
    c.codec = (h.codecs & kCodecHEVC) ? kCodecHEVC : kCodecH264;
    // Encoders need even dimensions; 16-aligned avoids padding/cropping work in the encoder.
    c.width = (h.eyeWidth * 2 + 15) & ~15u;
    c.height = (h.eyeHeight + 15) & ~15u;
    c.refreshHz = h.refreshHz;
    c.bitrateKbps = std::min<uint32_t>(h.maxBitrateKbps, 100'000);
    return c;
}

void StreamServer::recvLoop() {
    std::array<uint8_t, kMaxDatagram> buf{};
    std::vector<uint8_t> payload;
    while (running_) {
        size_t n = 0;
        net::Endpoint from;
        if (sock_.receive(buf, n, from, 100) != net::RecvStatus::Ok) continue;
        PacketHeader h;
        std::span<const uint8_t> body;
        if (decodePacket({buf.data(), n}, h, body) != DecodeError::None) continue;
        auto msg = decodeMessage(body);
        if (!msg) continue;

        if (auto* hello = std::get_if<StreamHello>(&*msg)) {
            StreamConfig cfg;
            {
                std::lock_guard l(mutex_);
                if (!client_ || !(*client_ == from)) log::info("stream: client {} connected", from.toString());
                client_ = from;
                if (!hello_) hello_ = *hello;  // display geometry is fixed for the session
                if (!config_) config_ = makeConfig(*hello_);
                cfg = *config_;
            }
            lastRecvUs_ = monotonicMicros();
            encodeMessage(cfg, payload);
            send(Channel::Control, payload);
            continue;
        }
        {
            std::lock_guard l(mutex_);
            if (!client_ || !(*client_ == from)) continue;  // only the client that said hello
        }
        lastRecvUs_ = monotonicMicros();
        if (auto* t = std::get_if<TrackingState>(&*msg)) {
            if (onTracking) onTracking(*t);
        } else if (std::get_if<IdrRequest>(&*msg)) {
            if (onIdrRequest) onIdrRequest();
        } else if (auto* s = std::get_if<ClientStats>(&*msg)) {
            if (onStats) onStats(*s);
        } else if (auto* p = std::get_if<Ping>(&*msg)) {
            Pong pong{p->pingId, p->clientSendUs, lastRecvUs_.load(), monotonicMicros()};
            encodeMessage(pong, payload);
            send(Channel::Control, payload);
        }
    }
}

void StreamServer::send(Channel ch, std::span<const uint8_t> payload) {
    std::optional<net::Endpoint> to;
    {
        std::lock_guard l(mutex_);
        to = client_;
    }
    if (!to) return;
    std::lock_guard l(sendMutex_);
    PacketHeader h;
    h.channel = ch;
    h.sequence = seq_[static_cast<int>(ch)]++;
    h.sendTimeUs = monotonicMicros();
    encodePacket(h, payload, packet_);
    sock_.sendTo(*to, packet_);
}

void StreamServer::sendVideo(uint32_t frameIndex, bool keyframe, const VideoFrameMeta& meta,
                             std::span<const uint8_t> bitstream) {
    frameBuf_.clear();
    encodeFrameMeta(meta, frameBuf_);
    frameBuf_.insert(frameBuf_.end(), bitstream.begin(), bitstream.end());
    fragmentFrame(frameIndex, meta.trackingTimeUs, keyframe ? kFrameKeyframe : 0, frameBuf_,
                  [&](std::span<const uint8_t> p) { send(Channel::Video, p); });
}

void StreamServer::sendHaptics(const Haptics& hp) {
    std::vector<uint8_t> payload;
    encodeMessage(hp, payload);
    send(Channel::Control, payload);
}

}  // namespace kage::driver
