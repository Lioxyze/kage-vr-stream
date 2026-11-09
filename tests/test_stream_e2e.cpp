// End-to-end test of the driver's streaming server with a fake headset, over real UDP
// on this PC (no SteamVR, no encoder): encrypted handshake, StreamConfig, session-start
// Wi-Fi probe (video held, then released with a keyframe request), sliced video frames
// with FEC reassembled intact, client stats delivered. The headset binds the PC's LAN
// address so the server treats it as Wi-Fi (FEC + probe), like a real Quest.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

#include "kage/crypto/secure.hpp"
#include "kage/net/udp_socket.hpp"
#include "kage/protocol/messages.hpp"
#include "kage/protocol/packet.hpp"
#include "kage/protocol/video_fragment.hpp"
#include "kage/util/time.hpp"
#include "stream_server.hpp"
#include "test_main.hpp"

using namespace kage;
namespace kp = kage::protocol;

namespace {

constexpr uint16_t kPort = 47912;  // not the real 47802: a running SteamVR must not interfere

// This PC's LAN IPv4 (UDP "connect" picks the outgoing interface, nothing is sent).
uint32_t lanIp() {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);  // TEST-NET, never contacted
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = ntohl(me.sin_addr.s_addr);
    }
    closesocket(s);
    return ip;
}

struct FakeHeadset {
    net::UdpSocket sock;
    net::Endpoint server;
    crypto::KeyPair id, eph;
    crypto::Key hostKey{};
    crypto::SecureSession session;
    std::vector<uint8_t> payload, packet, plain;
    uint16_t seq = 0;

    void send(kp::Channel ch, const kp::Message& m) {
        kp::encodeMessage(m, payload);
        kp::PacketHeader h;
        h.channel = ch;
        h.sequence = seq++;
        h.sendTimeUs = monotonicMicros();
        if (std::holds_alternative<kp::SecureHello>(m)) kp::encodePacket(h, payload, packet);
        else session.seal(h, payload, packet);
        sock.sendTo(server, packet);
    }
    // Next decrypted datagram (channel + plaintext), or false on timeout.
    bool recv(kp::Channel& ch, int timeoutMs) {
        std::array<uint8_t, kp::kMaxDatagram> buf{};
        const uint64_t end = monotonicMicros() + timeoutMs * 1000ull;
        while (monotonicMicros() < end) {
            size_t n = 0;
            net::Endpoint from;
            if (sock.receive(buf, n, from, 5) != net::RecvStatus::Ok) continue;
            kp::PacketHeader h;
            std::span<const uint8_t> body;
            if (kp::decodePacket({buf.data(), n}, h, body) != kp::DecodeError::None) continue;
            if (!(h.flags & kp::kFlagEncrypted)) {
                auto m = kp::decodeMessage(body);
                auto* sa = m ? std::get_if<kp::SecureAccept>(&*m) : nullptr;
                if (!sa || session.active()) continue;
                auto keys = crypto::deriveClient(id, eph, hostKey, sa->hostEphemeral);
                if (keys && crypto::equal16(keys->confirm, sa->confirm)) session.init(*keys);
                continue;
            }
            if (!session.open({buf.data(), n}, h, body, plain)) continue;
            ch = h.channel;
            return true;
        }
        return false;
    }
};

}  // namespace

TEST(fake_headset_end_to_end) {
    const uint32_t ip = lanIp();
    if (!ip || (ip >> 24) == 127) {
        std::printf("  pas d'adresse réseau locale : test ignoré\n");
        return;
    }
    // Throwaway keys in a temp folder (never the user's %LOCALAPPDATA%\KageVR).
    const auto dir = std::filesystem::temp_directory_path() / "kage-e2e-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    FakeHeadset hs;
    hs.id = *crypto::KeyPair::generate();
    hs.eph = *crypto::KeyPair::generate();
    REQUIRE(crypto::addTrusted(dir / "paired.txt", hs.id.pub, "fake-headset"));

    driver::StreamServer server;
    std::atomic<uint32_t> probeKbps{0}, idrRequests{0}, stats{0};
    server.onProbe = [&](uint32_t k) { probeKbps = k; };
    server.onIdrRequest = [&] { ++idrRequests; };
    server.onStats = [&](const kp::ClientStats&) { ++stats; };
    REQUIRE(server.start(kPort, dir));
    server.setEncoderFeatures(kp::kFeatureSliceUnits | kp::kFeatureIntraRefresh);  // as AMF reports it
    hs.hostKey = crypto::loadOrCreateKeyPair(dir / "host.key")->pub;
    auto s = net::UdpSocket::open(0, ip, false);
    REQUIRE(s.has_value());
    hs.sock = std::move(*s);
    hs.sock.setBufferSizes(256 * 1024, 4 * 1024 * 1024);
    hs.server = {ip, kPort};

    // 1. Encrypted handshake.
    for (int i = 0; i < 20 && !hs.session.active(); ++i) {
        hs.send(kp::Channel::Control, kp::SecureHello{hs.id.pub, hs.eph.pub});
        kp::Channel ch;
        hs.recv(ch, 100);
    }
    REQUIRE(hs.session.active());

    // 2. Features + StreamHello -> StreamConfig; the probe starts right after.
    kp::StreamHello hello;
    hello.eyeWidth = 1680;
    hello.eyeHeight = 1760;
    hello.refreshHz = 90;
    hs.send(kp::Channel::Control, kp::StreamFeatures{kp::kFeatureSliceUnits | kp::kFeatureIntraRefresh, 0});
    hs.send(kp::Channel::Control, hello);
    std::optional<kp::StreamConfig> cfg;
    kp::ProbeResult probe;
    uint64_t probeFirst = 0, probeLast = 0;
    bool probeReported = false;
    const uint64_t t0 = monotonicMicros();
    while (monotonicMicros() - t0 < 2'000'000 && !probeReported) {
        kp::Channel ch;
        if (!hs.recv(ch, 50)) {
            if (probeFirst && monotonicMicros() - probeFirst > 400'000) {  // tail lost: report what came
                probe.spanUs = static_cast<uint32_t>(probeLast - probeFirst);
                hs.send(kp::Channel::Control, probe);
                probeReported = true;
            }
            continue;
        }
        auto m = kp::decodeMessage(hs.plain);
        if (!m) continue;
        if (auto* c = std::get_if<kp::StreamConfig>(&*m)) cfg = *c;
        if (auto* bp = std::get_if<kp::BandwidthProbe>(&*m)) {
            const uint64_t now = monotonicMicros();
            if (!probeFirst || bp->probeId != probe.probeId) {
                probe = {};
                probe.probeId = bp->probeId;
                probeFirst = now;
            } else {
                probe.bytes += static_cast<uint32_t>(hs.plain.size());
            }
            ++probe.received;
            probeLast = now;
            if (bp->index + 1 == bp->count) {
                probe.spanUs = static_cast<uint32_t>(probeLast - probeFirst);
                hs.send(kp::Channel::Control, probe);
                probeReported = true;
            }
        }
    }
    REQUIRE(cfg.has_value());
    CHECK(cfg->width > 0 && cfg->height > 0);
    CHECK(probeReported);
    std::printf("  sonde : %u/2000 paquets en %.1f ms\n", probe.received, probe.spanUs / 1000.0);

    // 3. Video sent while the probe result is in flight is held; the result releases it
    //    with exactly one keyframe request.
    std::vector<uint8_t> slice(30000);
    std::mt19937 rng(7);
    for (auto& b : slice) b = static_cast<uint8_t>(rng());
    kp::VideoFrameMeta meta;
    meta.trackingTimeUs = 1234;
    for (int i = 0; i < 300 && !idrRequests; ++i) {
        server.sendVideo(1, true, meta, slice, 0, true);  // must be dropped while held
        kp::Channel ch;
        while (hs.recv(ch, 1)) CHECK(ch != kp::Channel::Video);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(idrRequests == 1);
    CHECK(probeKbps > 0);
    std::printf("  débit mesuré par la sonde : %u Mbit/s\n", probeKbps.load() / 1000);

    // 4. Released: 30 frames of 4 slices each, with FEC, reassembled byte-exact.
    kp::FrameReassembler reasm;
    int complete = 0, intact = 0, videoDatagrams = 0;
    for (uint32_t f = 10; f < 40; ++f) {
        videoDatagrams = 0;
        for (uint8_t sl = 0; sl < 4; ++sl) server.sendVideo(f, f == 10, meta, slice, sl, sl == 3);
        kp::Channel ch;
        while (hs.recv(ch, 20)) {
            if (ch != kp::Channel::Video) continue;
            // Simulated Wi-Fi loss: 2 datagrams per frame, in different slices (~30 each): FEC must repair.
            ++videoDatagrams;
            if (videoDatagrams == 5 || videoDatagrams == 70) continue;
            auto fr = reasm.push(hs.plain);
            if (!fr) continue;
            ++complete;
            const size_t off = (fr->frameIndex % kp::kMaxSlices == 0) ? kp::VideoFrameMeta::kSize : 0;
            if (fr->data.size() == slice.size() + off && std::memcmp(fr->data.data() + off, slice.data(), slice.size()) == 0)
                ++intact;
        }
    }
    CHECK(complete == 30 * 4);
    CHECK(intact == complete);
    CHECK(reasm.stats().fragmentsRecovered > 0);
    std::printf("  vidéo : %d tranches reçues, %d intactes, %llu fragments réparés par la FEC\n", complete, intact,
                static_cast<unsigned long long>(reasm.stats().fragmentsRecovered));

    // 5. Stats reach the driver.
    hs.send(kp::Channel::Control, kp::ClientStats{30, 0, 3000, 5000, 4000});
    for (int i = 0; i < 50 && !stats; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(stats == 1);

    server.stop();
    std::filesystem::remove_all(dir);
}

KT_MAIN()
