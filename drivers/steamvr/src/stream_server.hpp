// UDP endpoint of the SteamVR driver (port 47802): receives StreamHello,
// TrackingState, IdrRequest and ClientStats from the headset; sends
// StreamConfig, fragmented video frames and haptics.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <filesystem>

#include "kage/crypto/secure.hpp"
#include "kage/net/udp_socket.hpp"
#include "kage/protocol/messages.hpp"
#include "kage/protocol/packet.hpp"

namespace kage::driver {

class StreamServer {
public:
    std::function<void(const protocol::TrackingState&)> onTracking;  // network thread
    std::function<void()> onIdrRequest;
    std::function<void(const protocol::ClientStats&)> onStats;
    std::function<void()> onCalibrateFullBody;
    std::function<void(uint32_t, std::span<const int16_t>)> onMic;  // headset microphone

    // dataDir holds host.key and paired.txt (shared with kage-host).
    bool start(uint16_t port, const std::filesystem::path& dataDir);
    void stop();

    // First StreamHello received (and the config we answered with).
    std::optional<protocol::StreamHello> hello() const;
    std::optional<protocol::StreamConfig> config() const;
    bool clientActive() const;  // a packet was received in the last 2 s

    void sendVideo(uint32_t frameIndex, bool keyframe, const protocol::VideoFrameMeta& meta,
                   std::span<const uint8_t> bitstream);
    void sendHaptics(const protocol::Haptics& h);
    void setPacingKbps(uint32_t kbps) { pacingKbps_ = kbps; }
    // Raw PCM on the audio channel: u32 sample rate (LE) + interleaved stereo int16.
    void sendAudio(uint32_t sampleRate, std::span<const int16_t> samples);

private:
    void recvLoop();
    void handleSecureHello(const protocol::SecureHello& h, const net::Endpoint& from);
    void send(protocol::Channel ch, std::span<const uint8_t> payload);
    protocol::StreamConfig makeConfig(const protocol::StreamHello& h) const;

    net::UdpSocket sock_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::optional<net::Endpoint> client_;
    std::optional<protocol::StreamHello> hello_;
    std::optional<protocol::StreamConfig> config_;
    std::atomic<uint64_t> lastRecvUs_{0};
    std::atomic<uint32_t> pacingKbps_{60000};
    std::mutex sendMutex_;  // also guards session_ (counter) and the send buffers
    crypto::KeyPair hostKey_;
    std::filesystem::path pairedFile_;
    crypto::SecureSession session_;
    protocol::PublicKey sessionEph_{};       // client ephemeral of the current session
    protocol::SecureAccept lastAccept_{};    // re-sent if the client repeats its hello
    uint32_t seq_[6] = {};
    std::vector<uint8_t> frameBuf_, packet_, audioBuf_;
};

}  // namespace kage::driver
