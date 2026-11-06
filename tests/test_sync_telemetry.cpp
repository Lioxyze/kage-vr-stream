#include <cmath>

#include "kage/sync/clock_sync.hpp"
#include "kage/telemetry/stats.hpp"
#include "test_main.hpp"

using namespace kage;

TEST(clock_offset_symmetric_path) {
    sync::ClockSync c;
    // Host clock = client + 5'000'000 us. One-way 2 ms each, host holds 100 us.
    const uint64_t t0 = 1'000'000, t1 = t0 + 2000 + 5'000'000, t2 = t1 + 100, t3 = t0 + 2000 + 100 + 2000;
    auto s = c.addSample(t0, t1, t2, t3);
    REQUIRE(s.has_value());
    CHECK(s->rttUs == 4000);
    CHECK(s->offsetUs == 5'000'000);
    CHECK(c.clientToHost(10) == 5'000'010);
    CHECK(c.hostToClient(5'000'010) == 10);
}

TEST(clock_negative_offset) {
    sync::ClockSync c;
    // Host clock behind client by 3 s.
    const uint64_t t0 = 10'000'000, t1 = t0 + 1000 - 3'000'000, t2 = t1, t3 = t0 + 2000;
    auto s = c.addSample(t0, t1, t2, t3);
    REQUIRE(s.has_value());
    CHECK(s->offsetUs == -3'000'000);
}

TEST(clock_min_rtt_filter_rejects_queuing_noise) {
    sync::ClockSync c(16);
    const int64_t trueOffset = 777;
    // Good sample: symmetric 1 ms legs.
    c.addSample(0, 1000 + trueOffset, 1000 + trueOffset, 2000);
    // Congested samples: 20 ms extra only on the return path => biased offset.
    for (int i = 1; i < 10; ++i) {
        const uint64_t t0 = static_cast<uint64_t>(i) * 100000;
        c.addSample(t0, t0 + 1000 + trueOffset, t0 + 1000 + trueOffset, t0 + 22000);
    }
    CHECK(c.bestRttUs() == 2000);
    CHECK(c.offsetUs() == trueOffset);
}

TEST(clock_rejects_impossible_samples) {
    sync::ClockSync c;
    CHECK(!c.addSample(100, 0, 0, 50));      // receive before send
    CHECK(!c.addSample(0, 0, 10000, 100));   // host hold longer than round trip
    CHECK(!c.hasEstimate());
}

TEST(rolling_stats) {
    telemetry::RollingStats s(4);
    for (double v : {1.0, 2.0, 3.0, 4.0, 5.0}) s.add(v);  // 1 evicted
    CHECK(s.count() == 4);
    CHECK(s.min() == 2.0 && s.max() == 5.0);
    CHECK(std::fabs(s.mean() - 3.5) < 1e-9);
    CHECK(s.percentile(0) == 2.0 && s.percentile(100) == 5.0);
}

TEST(jitter_rfc3550) {
    telemetry::JitterEstimator j;
    for (int i = 0; i < 100; ++i) j.add(i * 11111, i * 11111 + 3000);  // constant transit
    CHECK(j.jitterUs() == 0.0);
    telemetry::JitterEstimator k;
    for (int i = 0; i < 2000; ++i) k.add(i * 11111, i * 11111 + ((i % 2) ? 4000 : 2000));  // alternating 2 ms
    CHECK(std::fabs(k.jitterUs() - 2000.0) < 1.0);  // converges to |D| = 2000
}

TEST(loss_counter_with_wrap) {
    telemetry::LossCounter l;
    for (uint32_t s : {0xFFFFFFFEu, 0xFFFFFFFFu, 1u, 2u}) l.add(s);  // 0 missing
    CHECK(l.expected() == 5);
    CHECK(l.lost() == 1);
}

KT_MAIN()
