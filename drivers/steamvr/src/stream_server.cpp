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
    if (probeThread_.joinable()) probeThread_.join();
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
    // Encoded size = render size x foveation factor (the centre stays 1:1, see foveationA), capped
    // at 4096 (HEVC/H.264 hardware limit): the headset samples normalised coordinates, so a
    // smaller encoded image is simply scaled. 16-aligned for the encoders.
    const float a = protocol::foveationA(foveation_.load());
    float eyeW = h.eyeWidth * a, eyeH = h.eyeHeight * a;
    const float fit = std::min({1.f, 4096.f / (2 * eyeW), 4096.f / eyeH});
    eyeW *= fit;
    eyeH *= fit;
    c.width = (static_cast<uint32_t>(eyeW) * 2 + 15) & ~15u;
    c.height = (static_cast<uint32_t>(eyeH) + 15) & ~15u;
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
                if (!config_) {
                    foveation_ = clientFoveation_.load();  // fixed for the stream once configured
                    config_ = makeConfig(*hello_);
                }
                cfg = *config_;
            }
            lastRecvUs_ = monotonicMicros();
            encodeMessage(cfg, payload);
            send(Channel::Control, payload);
            sendFeatures();
            // Wi-Fi: measure the link once per session so the video starts at a rate it can carry.
            if (fec_ && !probeStarted_.exchange(true)) {
                if (probeThread_.joinable()) probeThread_.join();
                probeThread_ = std::thread(&StreamServer::runProbe, this, ++probeId_);
            }
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
            sendFeatures();  // cheap (1 byte), keeps the headset in sync once the encoder is up
        } else if (auto* fe = std::get_if<StreamFeatures>(&*msg)) {
            clientFeatures_ = fe->flags;
            clientFoveation_ = fe->foveation;
        } else if (auto* pr = std::get_if<ProbeResult>(&*msg)) {
            if (pr->probeId == probeId_ && pr->received >= 20 && pr->spanUs >= 5000 && !probeKbps_) {
                const uint32_t kbps = static_cast<uint32_t>(static_cast<uint64_t>(pr->bytes) * 8000 / pr->spanUs);
                probeKbps_ = kbps;
                log::info("stream: Wi-Fi probe {}/{} datagrams in {:.1f} ms -> {} Mbit/s", pr->received, kProbeCount,
                          pr->spanUs / 1000.0, kbps / 1000);
                if (onProbe) onProbe(kbps);
            }
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
        fec_ = (from.ipv4 >> 24) != 127;  // USB bridge (loopback) never loses packets
        clientFeatures_ = 0;  // the headset re-announces its features in this session
        probeStarted_ = false;
        probeKbps_ = 0;
        log::info("stream: encrypted session with {} established, Wi-Fi priority {}", from.toString(),
                  sock_.setLowLatencyPriority(from) ? "on" : "unavailable");
    }
    encodeMessage(lastAccept_, payload);
    PacketHeader h;
    h.channel = Channel::Control;
    h.sendTimeUs = monotonicMicros();
    encodePacket(h, payload, out);
    sock_.sendTo(from, out);
}

void StreamServer::sendVideo(uint32_t frameIndex, bool keyframe, const VideoFrameMeta& meta,
                             std::span<const uint8_t> bitstream, uint8_t slice, bool lastSlice) {
    if (slice != 0 && slice != nextSlice_) {  // a slice went missing on the PC side: skip the frame
        nextSlice_ = 0;
        return;
    }
    nextSlice_ = lastSlice ? 0 : static_cast<uint8_t>(slice + 1);
    const bool units = (activeFeatures() & kFeatureSliceUnits) != 0;
    if (slice == 0) frameBuf_.clear();
    if (slice == 0 || units) {
        if (units) frameBuf_.clear();
        if (slice == 0) encodeFrameMeta(meta, frameBuf_);  // the meta travels with the first slice
    }
    frameBuf_.insert(frameBuf_.end(), bitstream.begin(), bitstream.end());
    if (!units && !lastSlice) return;  // older headset: slices are joined into one frame
    uint8_t flags = keyframe ? kFrameKeyframe : 0;
    uint32_t unit = frameIndex;
    if (units) {
        if (slice >= kMaxSlices) return;  // never happens with the encoder's 4 slices
        unit = frameIndex * kMaxSlices + slice;
        flags |= kFrameSliced | (lastSlice ? kFrameSliceLast : 0);
    }
    // Pacing: a whole frame sent back-to-back overflows Wi-Fi queues (losses, latency
    // spikes). Spread it at 4x the video bitrate (>= 250 Mbit/s): a few ms per frame.
    // The pacing clock runs across slices so back-to-back slices stay spread too.
    const double bps = std::max(250e6, 4.0 * pacingKbps_.load() * 1000.0);
    const uint64_t t0 = std::max(monotonicMicros(), paceUntilUs_);
    size_t sent = 0;
    fragmentFrame(unit, meta.trackingTimeUs, flags, frameBuf_,
                  [&](std::span<const uint8_t> p) {
                      const uint64_t due = t0 + static_cast<uint64_t>(sent * 8 * 1e6 / bps);
                      while (monotonicMicros() < due) std::this_thread::yield();
                      send(Channel::Video, p);
                      sent += p.size() + PacketHeader::kSize + crypto::SecureSession::kOverhead;
                  },
                  kMaxVideoPayload, fec_ ? kFecGroup : 0);
    paceUntilUs_ = t0 + static_cast<uint64_t>(sent * 8 * 1e6 / bps);
}

void StreamServer::runProbe(uint32_t id) {
    // kProbeCount full-size datagrams paced at kProbeMbps: above any video rate we use, so the
    // headset sees what the Wi-Fi really carries (about 4 MB, ~100 ms, once per session).
    std::vector<uint8_t> payload;
    BandwidthProbe p{id, 0, kProbeCount, static_cast<uint16_t>(kMaxVideoPayload - 9)};
    const double usPerDatagram = (kMaxVideoPayload + PacketHeader::kSize + crypto::SecureSession::kOverhead) * 8.0 / kProbeMbps;
    const uint64_t t0 = monotonicMicros();
    for (uint16_t i = 0; i < kProbeCount && running_; ++i) {
        const uint64_t due = t0 + static_cast<uint64_t>(i * usPerDatagram);
        while (monotonicMicros() < due) std::this_thread::yield();
        p.index = i;
        encodeMessage(p, payload);
        send(Channel::Control, payload);
    }
}

uint8_t StreamServer::activeFeatures() const {
    // Intra refresh is the encoder's; slice units also need a headset that understands them.
    return encoderFeatures_.load() & (kFeatureIntraRefresh | clientFeatures_.load());
}

void StreamServer::sendFeatures() {
    std::vector<uint8_t> payload;
    encodeMessage(StreamFeatures{activeFeatures(), foveation_.load()}, payload);
    send(Channel::Control, payload);
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
