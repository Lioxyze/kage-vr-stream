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
    std::function<void(uint32_t kbps)> onProbe;  // Wi-Fi throughput measured at session start

    // dataDir holds host.key and paired.txt (shared with kage-host).
    bool start(uint16_t port, const std::filesystem::path& dataDir);
    void stop();

    // First StreamHello received (and the config we answered with).
    std::optional<protocol::StreamHello> hello() const;
    std::optional<protocol::StreamConfig> config() const;
    bool clientActive() const;  // a packet was received in the last 2 s

    void sendVideo(uint32_t frameIndex, bool keyframe, const protocol::VideoFrameMeta& meta,
                   std::span<const uint8_t> bitstream, uint8_t slice = 0, bool lastSlice = true);
    // Features the encoder really enabled (kFeature*); combined with what the headset supports.
    void setEncoderFeatures(uint8_t f) { encoderFeatures_ = f; }
    uint8_t activeFeatures() const;
    float foveationA() const { return protocol::foveationA(foveation_); }
    void sendHaptics(const protocol::Haptics& h);
    void setPacingKbps(uint32_t kbps) { pacingKbps_ = kbps; }
    uint32_t probeKbps() const { return probeKbps_; }  // last session-start measure (0 = none)
    // Raw PCM on the audio channel: u32 sample rate (LE) + interleaved stereo int16.
    void sendAudio(uint32_t sampleRate, std::span<const int16_t> samples);

private:
    void recvLoop();
    void handleSecureHello(const protocol::SecureHello& h, const net::Endpoint& from);
    void send(protocol::Channel ch, std::span<const uint8_t> payload);
    void sendFeatures();
    void runProbe(uint32_t id);
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
    static constexpr unsigned kFecGroup = 8;  // 1 parity per 8 data datagrams (+12.5 % traffic)
    static constexpr uint16_t kProbeCount = 2000;
    static constexpr double kProbeMbps = 200;
    std::atomic<bool> fec_{true};  // FEC parity on Wi-Fi; off on the USB bridge (loopback, lossless)
    std::atomic<uint8_t> clientFeatures_{0}, encoderFeatures_{0};
    std::atomic<uint8_t> clientFoveation_{0}, foveation_{0};  // wanted by the headset / applied
    std::thread probeThread_;
    std::atomic<bool> probeStarted_{false};  // once per session
    std::atomic<uint32_t> probeId_{0}, probeKbps_{0};
    uint64_t paceUntilUs_ = 0;  // encoder thread only
    uint8_t nextSlice_ = 0;
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
