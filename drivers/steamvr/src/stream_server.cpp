#include "stream_server.hpp"
#include "settings.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "kage/protocol/packet.hpp"
#include "kage/protocol/video_fragment.hpp"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace kage::driver {

using namespace kage::protocol;

bool StreamServer::start(uint16_t port, const std::filesystem::path& dataDir) {
    auto key = crypto::loadOrCreateKeyPair(dataDir / L"host.key");
    if (!key) {
        log::error("stream: cannot load or create the PC key in {}", dataDir.string());
        return false;
    }
    hostKey_ = *key;
    pairedFile_ = dataDir / L"paired.txt";
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
    const auto st = kage::driver::loadSettings();
    const bool wantH264 = st.codec == "h264" || !(h.codecs & kCodecHEVC);
    c.codec = wantH264 && (h.codecs & kCodecH264) ? kCodecH264 : kCodecHEVC;
    // Encoders need even dimensions; 16-aligned avoids padding/cropping work in the encoder.
    c.width = (h.eyeWidth * 2 + 15) & ~15u;
    c.height = (h.eyeHeight + 15) & ~15u;
    c.refreshHz = h.refreshHz;
    c.bitrateKbps = st.maxBitrateKbps ? st.maxBitrateKbps : h.maxBitrateKbps;  // no cap: user choice
    return c;
}

void StreamServer::recvLoop() {
    std::array<uint8_t, kMaxDatagram> buf{};
    std::vector<uint8_t> payload, plain;
    while (running_) {
        size_t n = 0;
        net::Endpoint from;
        if (sock_.receive(buf, n, from, 100) != net::RecvStatus::Ok) continue;
        PacketHeader h;
        std::span<const uint8_t> body;
        if (decodePacket({buf.data(), n}, h, body) != DecodeError::None) continue;
        if (!(h.flags & kFlagEncrypted)) {
            // Only the key exchange travels in clear; everything else must be sealed.
            auto msg = decodeMessage(body);
            if (auto* sh = msg ? std::get_if<SecureHello>(&*msg) : nullptr) handleSecureHello(*sh, from);
            continue;
        }
        {
            std::lock_guard l(mutex_);
            if (!client_ || !(*client_ == from)) continue;
        }
        // open() is only ever called from this thread; init() happens on this thread too.
        if (!session_.open({buf.data(), n}, h, body, plain)) continue;
        if (h.channel == Channel::Audio) {  // headset microphone: u32 rate + mono int16
            if (plain.size() > 4 && (plain.size() - 4) % 2 == 0 && onMic) {
                const uint32_t rate = plain[0] | plain[1] << 8 | plain[2] << 16 | static_cast<uint32_t>(plain[3]) << 24;
                onMic(rate, {reinterpret_cast<const int16_t*>(plain.data() + 4), (plain.size() - 4) / 2});
            }
            continue;
        }
        auto msg = decodeMessage(plain);
        if (!msg) continue;

        if (auto* hello = std::get_if<StreamHello>(&*msg)) {
            StreamConfig cfg;
            {
                std::lock_guard l(mutex_);
                if (!hello_) hello_ = *hello;  // display geometry is fixed for the session
                if (!config_) config_ = makeConfig(*hello_);
                cfg = *config_;
            }
            lastRecvUs_ = monotonicMicros();
            encodeMessage(cfg, payload);
            send(Channel::Control, payload);
            continue;
        }
        lastRecvUs_ = monotonicMicros();
        if (auto* t = std::get_if<TrackingState>(&*msg)) {
            if (onTracking) onTracking(*t);
        } else if (std::get_if<IdrRequest>(&*msg)) {
            if (onIdrRequest) onIdrRequest();
        } else if (std::get_if<CalibrateFullBody>(&*msg)) {
            if (onCalibrateFullBody) onCalibrateFullBody();
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
    if (!session_.active()) return;  // never send video/tracking/audio in clear
    PacketHeader h;
    h.channel = ch;
    h.sequence = seq_[static_cast<int>(ch)]++;
    h.sendTimeUs = monotonicMicros();
    session_.seal(h, payload, packet_);
    sock_.sendTo(*to, packet_);
}

void StreamServer::handleSecureHello(const SecureHello& sh, const net::Endpoint& from) {
    if (!crypto::isTrusted(pairedFile_, sh.clientStatic)) {
        log::warn("stream: hello from unpaired headset {} ignored", from.toString());
        return;
    }
    std::vector<uint8_t> payload, out;
    std::lock_guard sl(sendMutex_);
    if (!(session_.active() && sh.clientEphemeral == sessionEph_)) {
        // New session (first connection or headset restarted): fresh ephemeral key.
        auto eph = crypto::KeyPair::generate();
        if (!eph) return;
        auto keys = crypto::deriveHost(hostKey_, *eph, sh.clientStatic, sh.clientEphemeral);
        if (!keys) return;
        lastAccept_.hostEphemeral = eph->pub;
        lastAccept_.confirm = keys->confirm;
        eph->wipe();
        session_.init(*keys);
        crypto::wipe(keys->send.data(), 32);
        crypto::wipe(keys->recv.data(), 32);
        sessionEph_ = sh.clientEphemeral;
        std::lock_guard l(mutex_);
        client_ = from;
        log::info("stream: encrypted session with {} established", from.toString());
    }
    encodeMessage(lastAccept_, payload);
    PacketHeader h;
    h.channel = Channel::Control;
    h.sendTimeUs = monotonicMicros();
    encodePacket(h, payload, out);
    sock_.sendTo(from, out);
}

void StreamServer::sendVideo(uint32_t frameIndex, bool keyframe, const VideoFrameMeta& meta,
                             std::span<const uint8_t> bitstream) {
    frameBuf_.clear();
    encodeFrameMeta(meta, frameBuf_);
    frameBuf_.insert(frameBuf_.end(), bitstream.begin(), bitstream.end());
    // Pacing: a whole frame sent back-to-back overflows Wi-Fi queues (losses, latency
    // spikes). Spread it at 4x the video bitrate (>= 250 Mbit/s): a few ms per frame.
    const double bps = std::max(250e6, 4.0 * pacingKbps_.load() * 1000.0);
    const uint64_t t0 = monotonicMicros();
    size_t sent = 0;
    fragmentFrame(frameIndex, meta.trackingTimeUs, keyframe ? kFrameKeyframe : 0, frameBuf_,
                  [&](std::span<const uint8_t> p) {
                      const uint64_t due = t0 + static_cast<uint64_t>(sent * 8 * 1e6 / bps);
                      while (monotonicMicros() < due) std::this_thread::yield();
                      send(Channel::Video, p);
                      sent += p.size() + PacketHeader::kSize + crypto::SecureSession::kOverhead;
                  });
}

void StreamServer::sendHaptics(const Haptics& hp) {
    std::vector<uint8_t> payload;
    encodeMessage(hp, payload);
    send(Channel::Control, payload);
}

void StreamServer::sendAudio(uint32_t sampleRate, std::span<const int16_t> samples) {
    audioBuf_.resize(4 + samples.size_bytes());
    for (int i = 0; i < 4; ++i) audioBuf_[i] = static_cast<uint8_t>(sampleRate >> (8 * i));
    std::memcpy(audioBuf_.data() + 4, samples.data(), samples.size_bytes());  // both ends little-endian
    send(Channel::Audio, audioBuf_);
}

}  // namespace kage::driver
