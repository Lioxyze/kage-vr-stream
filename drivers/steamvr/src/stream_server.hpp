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

#include "kage/net/udp_socket.hpp"
#include "kage/protocol/messages.hpp"
#include "kage/protocol/packet.hpp"

namespace kage::driver {

class StreamServer {
public:
    std::function<void(const protocol::TrackingState&)> onTracking;  // network thread
    std::function<void()> onIdrRequest;
    std::function<void(const protocol::ClientStats&)> onStats;

    bool start(uint16_t port);
    void stop();

    // First StreamHello received (and the config we answered with).
    std::optional<protocol::StreamHello> hello() const;
    std::optional<protocol::StreamConfig> config() const;
    bool clientActive() const;  // a packet was received in the last 2 s

    void sendVideo(uint32_t frameIndex, bool keyframe, const protocol::VideoFrameMeta& meta,
                   std::span<const uint8_t> bitstream);
    void sendHaptics(const protocol::Haptics& h);

private:
    void recvLoop();
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
    std::mutex sendMutex_;
    uint32_t seq_[6] = {};
    std::vector<uint8_t> frameBuf_, packet_;
};

}  // namespace kage::driver
