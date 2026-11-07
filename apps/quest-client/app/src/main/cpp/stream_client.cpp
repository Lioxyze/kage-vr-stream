#include "stream_client.hpp"

#include <aaudio/AAudio.h>

#include <chrono>
#include <vector>

#include "kage/protocol/packet.hpp"
#include "kage/util/log.hpp"
#include "kage/util/time.hpp"

namespace kp = kage::protocol;
namespace klog = kage::log;
using kage::monotonicMicros;

void StreamClient::start(const kage::net::Endpoint& driver, const kp::StreamHello& hello,
                         const kage::crypto::KeyPair& identity, const kage::crypto::Key& hostKey) {
    stop();
    auto eph = kage::crypto::KeyPair::generate();  // fresh per session: forward secrecy
    if (!eph) {
        klog::error("stream: no randomness for the session key");
        return;
    }
    identity_ = identity;
    ephemeral_ = *eph;
    hostKey_ = hostKey;
    session_.reset();
    secured_ = false;
    auto s = kage::net::UdpSocket::open(0, 0, false);
    if (!s) {
        klog::error("stream: cannot open socket");
        return;
    }
    sock_ = std::move(*s);
    sock_.setBufferSizes(256 * 1024, 4 * 1024 * 1024);  // absorb a full frame burst
    driver_ = driver;
    if (!sock_.setLowLatencyPriority(driver)) klog::warn("stream: Wi-Fi priority unavailable");
    hello_ = hello;
    {
        std::lock_guard l(mutex_);
        config_.reset();
        stats_ = {};
    }
    running_ = true;
    thread_ = std::thread(&StreamClient::loop, this);
    micThread_ = std::thread(&StreamClient::micLoop, this);
}

void StreamClient::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (micThread_.joinable()) micThread_.join();
    audio_.close();
    decoder_ = nullptr;
    session_.reset();
    ephemeral_.wipe();
    secured_ = false;
}

std::optional<kp::StreamConfig> StreamClient::config() const {
    std::lock_guard l(mutex_);
    return config_;
}

bool StreamClient::videoFlowing() const {
    const uint64_t t = lastFrameUs_;
    return t && monotonicMicros() - t < 500'000;
}

kp::VideoFrameMeta StreamClient::meta(uint32_t i) const {
    std::lock_guard l(mutex_);
    return metas_[i % metas_.size()];
}

StreamStats StreamClient::stats() const {
    std::lock_guard l(mutex_);
    return stats_;
}

std::optional<kp::Haptics> StreamClient::popHaptics() {
    std::lock_guard l(mutex_);
    if (haptics_.empty()) return std::nullopt;
    auto h = haptics_.front();
    haptics_.pop_front();
    return h;
}

void StreamClient::send(kp::Channel ch, const kp::Message& m) {
    std::lock_guard l(sendMutex_);
    kp::encodeMessage(m, payload_);
    kp::PacketHeader h;
    h.channel = ch;
    h.sequence = seq_[static_cast<int>(ch)]++;
    h.sendTimeUs = monotonicMicros();
    if (std::holds_alternative<kp::SecureHello>(m)) {
        kp::encodePacket(h, payload_, packet_);  // key exchange: the only clear message
    } else {
        if (!session_.active()) return;
        session_.seal(h, payload_, packet_);
    }
    sock_.sendTo(driver_, packet_);
}

void StreamClient::sendTracking(const kp::TrackingState& t) {
    if (running_) send(kp::Channel::Tracking, t);
}

void StreamClient::loop() {
    std::array<uint8_t, kp::kMaxDatagram> buf{};
    kp::FrameReassembler reasm;
    bool waitKey = true;
    uint64_t lastHello = 0, lastPing = 0, lastIdr = 0, lastStats = monotonicMicros();
    uint32_t pingId = 1, frames = 0, dropped = 0;
    uint64_t bytes = 0, netUsSum = 0, netCount = 0;
    std::array<uint64_t, 64> pingSent{};
    uint64_t lastDropped = 0;

    while (running_) {
        const uint64_t now = monotonicMicros();
        if (!secured_ && now - lastHello > 250'000) {
            send(kp::Channel::Control, kp::SecureHello{identity_.pub, ephemeral_.pub});
            lastHello = now;
        } else if (secured_ && !config() && now - lastHello > 250'000) {
            send(kp::Channel::Control, hello_);
            lastHello = now;
        }
        if (secured_ && now - lastPing > 250'000) {
            pingSent[pingId % 64] = now;
            send(kp::Channel::Control, kp::Ping{pingId++, now});
            lastPing = now;
        }
        if (now - lastStats > 1'000'000) {
            kp::ClientStats cs;
            cs.framesReceived = frames;
            cs.framesDropped = dropped;
            auto* dec = decoder_.load();
            cs.decodeUsAvg = dec ? dec->takeDecodeUsAvg() : 0;
            cs.networkUsAvg = netCount ? static_cast<uint32_t>(netUsSum / netCount) : 0;
            cs.rttUs = clock_.hasEstimate() ? static_cast<uint32_t>(clock_.lastRttUs()) : 0;
            send(kp::Channel::Control, cs);
            klog::info("stream: {} frames {} dropped {:.1f} Mbit/s rtt {:.1f} ms decode {:.2f} ms", frames, dropped,
                       bytes * 8 / 1e6, cs.rttUs / 1000.0, cs.decodeUsAvg / 1000.0);
            {
                std::lock_guard l(mutex_);
                const float secs = (now - lastStats) / 1e6f;
                stats_.fps = frames / secs;
                stats_.mbps = bytes * 8 / secs / 1e6f;
                stats_.rttMs = clock_.hasEstimate() ? clock_.lastRttUs() / 1000.f : -1;
                stats_.decodeMs = cs.decodeUsAvg ? cs.decodeUsAvg / 1000.f : -1;
                stats_.networkMs = netCount ? cs.networkUsAvg / 1000.f : -1;
                stats_.dropped = dropped;
            }
            frames = dropped = 0;
            bytes = netUsSum = netCount = 0;
            lastStats = now;
        }

        size_t n = 0;
        kage::net::Endpoint from;
        if (sock_.receive(buf, n, from, 5) != kage::net::RecvStatus::Ok || !(from == driver_)) continue;
        const uint64_t recvUs = monotonicMicros();
        kp::PacketHeader h;
        std::span<const uint8_t> body;
        if (kp::decodePacket({buf.data(), n}, h, body) != kp::DecodeError::None) continue;
        if (!(h.flags & kp::kFlagEncrypted)) {
            auto m = kp::decodeMessage(body);
            const auto* sa = m ? std::get_if<kp::SecureAccept>(&*m) : nullptr;
            if (!sa || secured_) continue;
            auto keys = kage::crypto::deriveClient(identity_, ephemeral_, hostKey_, sa->hostEphemeral);
            if (!keys || !kage::crypto::equal16(keys->confirm, sa->confirm)) {
                klog::warn("stream: the PC did not prove its paired key, ignoring");
                continue;
            }
            {
                std::lock_guard l(sendMutex_);
                session_.init(*keys);
            }
            kage::crypto::wipe(&*keys, sizeof *keys);
            secured_ = true;
            lastHello = 0;  // send the StreamHello right away
            klog::info("stream: encrypted session established");
            continue;
        }
        if (!session_.open({buf.data(), n}, h, body, plain_)) continue;
        body = plain_;

        if (h.channel == kp::Channel::Video) {
            bytes += n;
            auto frame = reasm.push(body);
            if (reasm.stats().framesDropped > lastDropped) {
                dropped += static_cast<uint32_t>(reasm.stats().framesDropped - lastDropped);
                lastDropped = reasm.stats().framesDropped;
            }
            if (reasm.keyframeNeeded()) {
                waitKey = true;  // references are broken: never decode on top of a hole
                if (recvUs - lastIdr > 100'000) {
                    send(kp::Channel::Control, kp::IdrRequest{0});
                    lastIdr = recvUs;
                }
                reasm.acknowledgeKeyframeRequest();
            }
            if (!frame) continue;
            kp::VideoFrameMeta meta;
            if (!kp::decodeFrameMeta(frame->data, meta)) continue;
            const bool key = frame->flags & kp::kFrameKeyframe;
            if (waitKey && !key) {
                ++dropped;
                if (recvUs - lastIdr > 100'000) {
                    send(kp::Channel::Control, kp::IdrRequest{frame->frameIndex});
                    lastIdr = recvUs;
                }
                continue;
            }
            waitKey = false;
            {
                std::lock_guard l(mutex_);
                metas_[frame->frameIndex % metas_.size()] = meta;
            }
            if (clock_.hasEstimate()) {
                const int64_t oneWay = static_cast<int64_t>(recvUs) - static_cast<int64_t>(clock_.hostToClient(h.sendTimeUs));
                if (oneWay > 0 && oneWay < 1'000'000) {
                    netUsSum += static_cast<uint64_t>(oneWay);
                    ++netCount;
                }
            }
            ++frames;
            lastFrameUs_ = recvUs;
            if (frames == 1 && !loggedFirst_) {
                loggedFirst_ = true;
                klog::info("first video frame {} ({} bytes, key {})", frame->frameIndex, frame->data.size(), key);
            }
            if (auto* dec = decoder_.load())
                dec->submit(frame->frameIndex, frame->data.data() + kp::VideoFrameMeta::kSize,
                            frame->data.size() - kp::VideoFrameMeta::kSize, recvUs);
            continue;
        }
        if (h.channel == kp::Channel::Audio) {
            // u32 sample rate + interleaved stereo int16 (whole frames only).
            if (body.size() < 8 || (body.size() - 4) % 4) continue;
            const uint32_t rate = body[0] | body[1] << 8 | body[2] << 16 | static_cast<uint32_t>(body[3]) << 24;
            audio_.push(rate, {reinterpret_cast<const int16_t*>(body.data() + 4), (body.size() - 4) / 2});
            continue;
        }
        auto msg = kp::decodeMessage(body);
        if (!msg) continue;
        if (auto* c = std::get_if<kp::StreamConfig>(&*msg)) {
            std::lock_guard l(mutex_);
            if (!config_) klog::info("stream config: {}x{} {} {:.0f} Hz {} Mbit/s", c->width, c->height,
                                     c->codec == kp::kCodecHEVC ? "HEVC" : "H.264", c->refreshHz, c->bitrateKbps / 1000);
            config_ = *c;
        } else if (auto* p = std::get_if<kp::Pong>(&*msg)) {
            if (pingSent[p->pingId % 64] == p->clientSendUs) clock_.addSample(p->clientSendUs, p->hostRecvUs, p->hostSendUs, recvUs);
        } else if (auto* hp = std::get_if<kp::Haptics>(&*msg)) {
            std::lock_guard l(mutex_);
            if (haptics_.size() < 16) haptics_.push_back(*hp);
        }
    }
}

// Microphone: AAudio capture, 10 ms chunks of 48 kHz mono int16, sent sealed on the
// Audio channel (u32 rate + samples). The stream is opened only while the user has
// enabled it, so the mic is never recorded without their knowledge.
void StreamClient::micLoop() {
    AAudioStream* in = nullptr;
    constexpr int32_t kRate = 48000, kChunk = 480;
    std::vector<uint8_t> pkt(4 + kChunk * 2);
    for (int i = 0; i < 4; ++i) pkt[i] = static_cast<uint8_t>(kRate >> (8 * i));
    while (running_) {
        const bool want = micOn_ && session_.active();
        if (want && !in) {
            AAudioStreamBuilder* b = nullptr;
            if (AAudio_createStreamBuilder(&b) == AAUDIO_OK) {
                AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
                AAudioStreamBuilder_setSampleRate(b, kRate);
                AAudioStreamBuilder_setChannelCount(b, 1);
                AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
                AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
                AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
                if (AAudioStreamBuilder_openStream(b, &in) != AAUDIO_OK || AAudioStream_requestStart(in) != AAUDIO_OK) {
                    if (in) AAudioStream_close(in);
                    in = nullptr;
                    klog::warn("mic: cannot open the microphone (permission?)");
                } else {
                    klog::info("mic: capturing {} Hz", AAudioStream_getSampleRate(in));
                }
                AAudioStreamBuilder_delete(b);
            }
            if (!in) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }
        if (!want && in) {
            AAudioStream_requestStop(in);
            AAudioStream_close(in);
            in = nullptr;
            klog::info("mic: stopped");
        }
        micActive_ = in != nullptr;
        if (!in) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const aaudio_result_t n = AAudioStream_read(in, pkt.data() + 4, kChunk, 100'000'000);
        if (n < 0) {  // device lost: reopen
            AAudioStream_close(in);
            in = nullptr;
            continue;
        }
        if (n == 0) continue;
        std::lock_guard l(sendMutex_);
        kp::PacketHeader h;
        h.channel = kp::Channel::Audio;
        h.sequence = seq_[static_cast<int>(kp::Channel::Audio)]++;
        h.sendTimeUs = monotonicMicros();
        if (!session_.active()) continue;
        session_.seal(h, std::span<const uint8_t>(pkt.data(), 4 + static_cast<size_t>(n) * 2), packet_);
        sock_.sendTo(driver_, packet_);
    }
    if (in) {
        AAudioStream_requestStop(in);
        AAudioStream_close(in);
    }
    micActive_ = false;
}
