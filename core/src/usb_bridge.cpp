#include "kage/net/usb_bridge.hpp"

#include <array>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "kage/net/udp_socket.hpp"
#include "kage/protocol/messages.hpp"
#include "kage/util/log.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Sock = SOCKET;
static constexpr Sock kBad = INVALID_SOCKET;
static void closeSock(Sock s) { closesocket(s); }
static constexpr int kSendFlags = 0;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using Sock = int;
static constexpr Sock kBad = -1;
static void closeSock(Sock s) { ::close(s); }
static constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

namespace kage::net {
namespace {

constexpr uint16_t kBase = protocol::kDefaultDiscoveryPort;  // 47800, 47801, 47802
constexpr int kPorts = 3;

sockaddr_in loopAddr(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(0x7F000001u);
    return a;
}

void tune(Sock s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    int buf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
}

bool readAll(Sock s, uint8_t* p, size_t n) {
    while (n) {
        const int r = recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// Relays one TCP connection until it drops. dest(i) = where datagrams of port i go;
// onRecv(i, from) is told the UDP sender (client side remembers it for replies).
template <class Dest, class OnRecv>
void pump(Sock tcp, std::array<UdpSocket, kPorts>& udp, const std::atomic<bool>& stop, Dest dest, OnRecv onRecv) {
    tune(tcp);
    std::atomic<bool> dead{false};
    std::mutex wm;
    std::vector<std::thread> readers;
    for (int i = 0; i < kPorts; ++i)
        readers.emplace_back([&, i] {
            std::vector<uint8_t> buf(3 + 65536);
            while (!stop && !dead) {
                size_t n = 0;
                Endpoint from;
                const auto st = udp[i].receive(std::span(buf).subspan(3, 65535), n, from, 100);
                if (st != RecvStatus::Ok || n == 0) continue;
                onRecv(i, from);
                buf[0] = static_cast<uint8_t>(i);
                buf[1] = static_cast<uint8_t>(n);
                buf[2] = static_cast<uint8_t>(n >> 8);
                std::lock_guard l(wm);
                const char* p = reinterpret_cast<const char*>(buf.data());
                size_t left = n + 3;
                while (left) {
                    const int w = send(tcp, p, static_cast<int>(left), kSendFlags);
                    if (w <= 0) {
                        dead = true;
                        break;
                    }
                    p += w;
                    left -= static_cast<size_t>(w);
                }
            }
        });
    std::vector<uint8_t> payload(65536);
    while (!stop && !dead) {
        uint8_t h[3];
        if (!readAll(tcp, h, 3)) break;
        const size_t n = h[1] | (h[2] << 8);
        if (h[0] >= kPorts || !readAll(tcp, payload.data(), n)) break;
        if (auto to = dest(h[0])) udp[h[0]].sendTo(*to, std::span<const uint8_t>(payload.data(), n));
    }
    dead = true;
    closeSock(tcp);
    for (auto& t : readers) t.join();
}

}  // namespace

void runUsbBridgeHost(const std::atomic<bool>& stop) {
    std::array<UdpSocket, kPorts> udp;
    for (auto& u : udp) {  // opening a UdpSocket also initialises Winsock
        auto s = UdpSocket::open(0, 0x7F000001u, false);
        if (!s) return;
        u = std::move(*s);
        u.setBufferSizes(4 * 1024 * 1024, 4 * 1024 * 1024);
    }
    Sock ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const auto a = loopAddr(kUsbBridgePort);
    if (ls == kBad || bind(ls, reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 || listen(ls, 1) != 0) {
        log::warn("usb: cannot listen on 127.0.0.1:{}", kUsbBridgePort);
        if (ls != kBad) closeSock(ls);
        return;
    }
    log::info("usb: bridge ready on 127.0.0.1:{} (adb reverse)", kUsbBridgePort);
    while (!stop) {
        fd_set f;
        FD_ZERO(&f);
        FD_SET(ls, &f);
        timeval tv{0, 200000};
        if (select(static_cast<int>(ls) + 1, &f, nullptr, nullptr, &tv) <= 0) continue;
        Sock c = accept(ls, nullptr, nullptr);
        if (c == kBad) continue;
        log::info("usb: headset connected over the cable");
        pump(c, udp, stop, [](int i) { return std::optional(Endpoint::loopback(static_cast<uint16_t>(kBase + i))); },
             [](int, const Endpoint&) {});
        log::info("usb: cable link closed");
    }
    closeSock(ls);
}

void runUsbBridgeClient(const std::atomic<bool>& stop, std::atomic<bool>& connected) {
    std::array<UdpSocket, kPorts> udp;
    for (int i = 0; i < kPorts; ++i) {
        auto s = UdpSocket::open(static_cast<uint16_t>(kBase + i), 0x7F000001u, false);
        if (!s) return;
        udp[i] = std::move(*s);
        udp[i].setBufferSizes(4 * 1024 * 1024, 4 * 1024 * 1024);
    }
    std::array<std::optional<Endpoint>, kPorts> peer;
    std::mutex pm;
    while (!stop) {
        Sock c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const auto a = loopAddr(kUsbBridgePort);
        if (c != kBad && connect(c, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
            connected = true;
            pump(
                c, udp, stop,
                [&](int i) {
                    std::lock_guard l(pm);
                    return peer[i];
                },
                [&](int i, const Endpoint& from) {
                    std::lock_guard l(pm);
                    peer[i] = from;
                });
            connected = false;
        } else if (c != kBad) {
            closeSock(c);
        }
        for (int i = 0; i < 20 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace kage::net
