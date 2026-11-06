// Integration test over real UDP sockets on 127.0.0.1 (no mocks):
// discovery probe/announce, then Ping/Pong RTT and clock offset.
#include <cstdlib>

#include "kage/session/client_link.hpp"
#include "kage/session/host_service.hpp"
#include "kage/telemetry/stats.hpp"
#include "test_main.hpp"

using namespace kage;

TEST(discovery_and_ping_over_loopback) {
    session::HostServiceConfig cfg;
    cfg.hostName = "loopback-host";
    cfg.discoveryPort = 0;  // ephemeral: test must not collide with a running host
    cfg.controlPort = 0;
    cfg.bindIp = 0x7F000001u;
    session::HostService host(cfg);
    REQUIRE(host.start());

    session::ClientLink client;
    REQUIRE(client.open());
    auto found = client.discover(net::Endpoint::loopback(host.discoveryPort()), "test-client", 1000);
    REQUIRE(found.has_value());
    CHECK(found->name == "loopback-host");
    CHECK(found->control.port == host.controlPort());

    telemetry::RollingStats rtt(200);
    for (int i = 0; i < 200; ++i)
        if (auto s = client.ping(found->control, 500)) rtt.add(static_cast<double>(s->rttUs));
    CHECK(rtt.count() == 200);
    // Same process => same clock: offset must be ~0 (bounded by measured RTT).
    CHECK(std::llabs(client.clock().offsetUs()) <= client.clock().bestRttUs() + 50);
    std::printf("  loopback RTT (measured): min %.0f us, median %.0f us, p99 %.0f us, max %.0f us\n", rtt.min(),
                rtt.percentile(50), rtt.percentile(99), rtt.max());
    std::printf("  estimated clock offset: %lld us\n", static_cast<long long>(client.clock().offsetUs()));
    host.stop();
    CHECK(host.counters().pingsAnswered == 200);
}

TEST(host_ignores_garbage) {
    session::HostServiceConfig cfg;
    cfg.discoveryPort = 0;
    cfg.controlPort = 0;
    cfg.bindIp = 0x7F000001u;
    session::HostService host(cfg);
    REQUIRE(host.start());
    auto sock = net::UdpSocket::open(0, 0x7F000001u);
    REQUIRE(sock.has_value());
    std::vector<uint8_t> junk(100, 0xAB);
    for (int i = 0; i < 50; ++i) sock->sendTo(net::Endpoint::loopback(host.controlPort()), junk);

    // Host must still answer a legit client afterwards.
    session::ClientLink client;
    REQUIRE(client.open());
    CHECK(client.ping(net::Endpoint::loopback(host.controlPort()), 1000).has_value());
    host.stop();
    CHECK(host.counters().packetsRejected == 50);
}

KT_MAIN()
