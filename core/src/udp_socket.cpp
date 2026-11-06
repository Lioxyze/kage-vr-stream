#include "kage/net/udp_socket.hpp"

#include <format>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kage::net {
namespace {

#ifdef _WIN32
struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};
bool ensureWinsock() {
    static WinsockInit init;
    return init.ok;
}
#else
bool ensureWinsock() { return true; }
#endif

sockaddr_in toSockaddr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = htonl(e.ipv4);
    return a;
}

}  // namespace

std::optional<Endpoint> Endpoint::parse(const std::string& ip, uint16_t port) {
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return std::nullopt;
    return Endpoint{ntohl(a.s_addr), port};
}

std::string Endpoint::toString() const {
    return std::format("{}.{}.{}.{}:{}", (ipv4 >> 24) & 0xFF, (ipv4 >> 16) & 0xFF, (ipv4 >> 8) & 0xFF, ipv4 & 0xFF,
                       port);
}

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : handle_(o.handle_) { o.handle_ = kInvalid; }

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        close();
        handle_ = o.handle_;
        o.handle_ = kInvalid;
    }
    return *this;
}

void UdpSocket::close() {
    if (handle_ == kInvalid) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(handle_));
#else
    ::close(handle_);
#endif
    handle_ = kInvalid;
}

std::optional<UdpSocket> UdpSocket::open(uint16_t port, uint32_t bindIp, bool allowBroadcast) {
    if (!ensureWinsock()) return std::nullopt;
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return std::nullopt;
    UdpSocket sock(static_cast<Handle>(s));
    // Without this, an ICMP "port unreachable" makes the next recvfrom fail with WSAECONNRESET.
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return std::nullopt;
    UdpSocket sock(s);
#endif
    if (allowBroadcast) {
        int on = 1;
        setsockopt(static_cast<decltype(s)>(sock.handle_), SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&on), sizeof on);
    }
    sockaddr_in a = toSockaddr({bindIp, port});
    if (::bind(static_cast<decltype(s)>(sock.handle_), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
        return std::nullopt;
    return sock;
}

uint16_t UdpSocket::localPort() const {
    sockaddr_in a{};
    socklen_t len = sizeof a;
#ifdef _WIN32
    if (getsockname(static_cast<SOCKET>(handle_), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
#else
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
#endif
    return ntohs(a.sin_port);
}

bool UdpSocket::sendTo(const Endpoint& to, std::span<const uint8_t> data) {
    sockaddr_in a = toSockaddr(to);
#ifdef _WIN32
    const int n = ::sendto(static_cast<SOCKET>(handle_), reinterpret_cast<const char*>(data.data()),
                           static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
#else
    const ssize_t n = ::sendto(handle_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
#endif
    return n == static_cast<decltype(n)>(data.size());
}

RecvStatus UdpSocket::receive(std::span<uint8_t> buffer, size_t& size, Endpoint& from, int timeoutMs) {
    size = 0;
#ifdef _WIN32
    WSAPOLLFD p{static_cast<SOCKET>(handle_), POLLRDNORM, 0};
    const int r = WSAPoll(&p, 1, timeoutMs);
#else
    pollfd p{handle_, POLLIN, 0};
    const int r = ::poll(&p, 1, timeoutMs);
#endif
    if (r == 0) return RecvStatus::Timeout;
    if (r < 0) return RecvStatus::Error;

    sockaddr_in a{};
    socklen_t len = sizeof a;
#ifdef _WIN32
    const int n = ::recvfrom(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(buffer.data()),
                             static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (n < 0) return RecvStatus::Error;  // includes WSAEMSGSIZE (datagram larger than buffer)
#else
    const ssize_t n = ::recvfrom(handle_, buffer.data(), buffer.size(), MSG_TRUNC, reinterpret_cast<sockaddr*>(&a), &len);
    if (n < 0) return RecvStatus::Error;
    if (static_cast<size_t>(n) > buffer.size()) return RecvStatus::Error;  // truncated
#endif
    size = static_cast<size_t>(n);
    from.ipv4 = ntohl(a.sin_addr.s_addr);
    from.port = ntohs(a.sin_port);
    return RecvStatus::Ok;
}

void UdpSocket::setBufferSizes(int sendBytes, int recvBytes) {
#ifdef _WIN32
    auto h = static_cast<SOCKET>(handle_);
#else
    auto h = handle_;
#endif
    setsockopt(h, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof sendBytes);
    setsockopt(h, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBytes), sizeof recvBytes);
}

std::string UdpSocket::lastErrorString() {
#ifdef _WIN32
    return std::format("WSA error {}", WSAGetLastError());
#else
    return std::format("errno {} ({})", errno, std::strerror(errno));
#endif
}

}  // namespace kage::net
