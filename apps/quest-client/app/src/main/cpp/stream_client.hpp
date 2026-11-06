// Headset side of the streaming link with the SteamVR driver (udp/47802).
#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

#include "kage/crypto/secure.hpp"
#include "kage/net/udp_socket.hpp"
#include "kage/protocol/messages.hpp"
#include "kage/protocol/video_fragment.hpp"
#include "kage/sync/clock_sync.hpp"
#include "audio_player.hpp"
#include "video_decoder.hpp"

struct StreamStats {
    float fps = 0;
    float mbps = 0;
    float rttMs = -1;
    float decodeMs = -1;
    float networkMs = -1;
    uint32_t dropped = 0;  // frames lost/skipped during the last second
};

class StreamClient {
public:
    // identity = this headset's key pair; hostKey = the paired PC's public key.
    void start(const kage::net::Endpoint& driver, const kage::protocol::StreamHello& hello,
               const kage::crypto::KeyPair& identity, const kage::crypto::Key& hostKey);
    bool secured() const { return secured_; }
    void stop();
    bool running() const { return running_; }

    std::optional<kage::protocol::StreamConfig> config() const;
    void attachDecoder(VideoDecoder* d) { decoder_ = d; }  // after config, on the render thread
    bool videoFlowing() const;  // a frame arrived in the last 500 ms

    void sendTracking(const kage::protocol::TrackingState& t);
    void requestFullBodyCalibration() {
        if (running_) send(kage::protocol::Channel::Control, kage::protocol::CalibrateFullBody{});
    }
    std::optional<kage::protocol::Haptics> popHaptics();
    kage::protocol::VideoFrameMeta meta(uint32_t frameIndex) const;
    StreamStats stats() const;

private:
    void loop();
    void send(kage::protocol::Channel ch, const kage::protocol::Message& m);

    kage::net::UdpSocket sock_;
    kage::net::Endpoint driver_;
    kage::protocol::StreamHello hello_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<VideoDecoder*> decoder_{nullptr};

    mutable std::mutex mutex_;
    std::optional<kage::protocol::StreamConfig> config_;
    std::array<kage::protocol::VideoFrameMeta, 64> metas_{};
    std::deque<kage::protocol::Haptics> haptics_;
    StreamStats stats_;
    std::atomic<uint64_t> lastFrameUs_{0};

    std::mutex sendMutex_;
    uint32_t seq_[6] = {};
    std::vector<uint8_t> payload_, packet_;
    kage::sync::ClockSync clock_;
    AudioPlayer audio_;  // fed and reopened from the network thread only

    kage::crypto::KeyPair identity_, ephemeral_;
    kage::crypto::Key hostKey_{};
    kage::crypto::SecureSession session_;  // seal under sendMutex_, open on the network thread
    std::atomic<bool> secured_{false};
    std::vector<uint8_t> plain_;
    bool loggedFirst_ = false;
};
