// KAGE VR Stream - minimal cross-platform IPv4 UDP socket (Winsock / BSD).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace kage::net {

struct Endpoint {
    uint32_t ipv4 = 0;  // host byte order, e.g. 0x7F000001 for 127.0.0.1
    uint16_t port = 0;

    static std::optional<Endpoint> parse(const std::string& ip, uint16_t port);
    static Endpoint loopback(uint16_t port) { return {0x7F000001u, port}; }
    static Endpoint broadcast(uint16_t port) { return {0xFFFFFFFFu, port}; }
    std::string toString() const;
    bool operator==(const Endpoint&) const = default;
};

enum class RecvStatus { Ok, Timeout, Error };

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& o) noexcept;
    UdpSocket& operator=(UdpSocket&& o) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds to bindIp:port (port 0 = ephemeral, bindIp 0 = all interfaces).
    static std::optional<UdpSocket> open(uint16_t port, uint32_t bindIp = 0, bool allowBroadcast = false);

    bool valid() const { return handle_ != kInvalid; }
    uint16_t localPort() const;

    // Returns false on error. Datagrams are sent whole or not at all.
    bool sendTo(const Endpoint& to, std::span<const uint8_t> data);

    // Waits up to timeoutMs (0 = poll, <0 = block). On Ok, `size` is the datagram length.
    // Datagrams larger than buffer are reported as Error (truncated data is never returned).
    RecvStatus receive(std::span<uint8_t> buffer, size_t& size, Endpoint& from, int timeoutMs);

    // Requests larger kernel buffers (best effort); useful for video bursts.
    void setBufferSizes(int sendBytes, int recvBytes);

    // Wi-Fi priority (WMM video queue) for traffic to `to`: fewer latency spikes on a busy
    // Wi-Fi. Windows: qWAVE AudioVideo flow (DSCP 40, no admin). Android: IP_TOS.
    // Best effort: false when the OS refuses (traffic still flows normally).
    bool setLowLatencyPriority(const Endpoint& to);

    static std::string lastErrorString();

private:
#ifdef _WIN32
    using Handle = uintptr_t;
    static constexpr Handle kInvalid = ~static_cast<uintptr_t>(0);
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    explicit UdpSocket(Handle h) : handle_(h) {}
    void close();
    Handle handle_ = kInvalid;
};

}  // namespace kage::net
