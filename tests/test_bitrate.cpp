// Bitrate controller scenarios (pure logic, simulated 250 ms reports at 60 fps).
#include "bitrate_controller.hpp"
#include "test_main.hpp"

using kage::driver::BitrateController;
using R = BitrateController::Report;

namespace {
constexpr uint64_t kStep = 250'000;

// Calm reports from t until t+secs; returns the time after.
uint64_t calm(BitrateController& c, uint64_t t, double secs, uint32_t net = 6000) {
    for (uint64_t end = t + static_cast<uint64_t>(secs * 1e6); t < end; t += kStep) c.update({t, 15, 0, 15, net});
    return t;
}
}  // namespace

TEST(in_flight_frames_are_not_loss) {
    BitrateController c(200'000, 100'000);
    uint64_t t = calm(c, kStep, 6);  // past start-up
    const uint32_t before = c.kbps();
    // Report boundary jitter: 3 frames arrive one report late, every other report.
    for (int i = 0; i < 40; ++i, t += kStep) c.update({t, i % 2 ? 18u : 12u, 0, 15, 6000});
    CHECK(c.kbps() >= before);  // never cut (old controller: cut to the 10 Mbit/s floor path)
}

TEST(frames_really_missing_are_cut) {
    BitrateController c(200'000, 100'000);
    uint64_t t = calm(c, kStep, 6);
    const uint32_t before = c.kbps();
    for (int i = 0; i < 4; ++i, t += kStep) c.update({t, 9, 0, 15, 6000});  // 6 of 15 never arrive
    CHECK(c.kbps() < before);
}

TEST(dropped_frames_cut_at_once) {
    BitrateController c(200'000, 100'000);
    uint64_t t = calm(c, kStep, 6);
    const uint32_t before = c.kbps();
    c.update({t, 14, 1, 15, 6000});
    CHECK(c.kbps() == before * 3 / 4);
}

TEST(lone_delay_spike_ignored_repeated_cut) {
    BitrateController c(200'000, 100'000);
    uint64_t t = calm(c, kStep, 6);
    const uint32_t before = c.kbps();
    c.update({t, 15, 0, 15, 45000});  // one 45 ms report
    t += kStep;
    CHECK(c.kbps() >= before);
    t = calm(c, t, 1);
    c.update({t, 15, 0, 15, 45000});
    c.update({t + kStep, 15, 0, 15, 45000});  // twice in a row: a real queue
    CHECK(c.kbps() < before);
}

TEST(quick_recovery_to_last_stable_rate) {
    BitrateController c(200'000, 120'000);
    uint64_t t = calm(c, kStep, 8);
    const uint32_t stable = c.kbps();
    c.update({t, 14, 1, 15, 6000});  // isolated loss: -25 %
    t += kStep;
    CHECK(c.kbps() < stable);
    t = calm(c, t, 3);  // 1 s hold + 2 s of +30 %/s
    CHECK(c.kbps() >= stable * 95 / 100);
}

TEST(startup_stalls_ignored_and_probe_rate_held) {
    BitrateController c(200'000, 60'000);
    for (uint64_t t = kStep; t < 4'000'000; t += kStep) c.update({t, 0, 0, 15, 0});  // probe hold, start-up
    CHECK(c.kbps() == 60'000);
    c.set(36'000, 4'000'000);
    calm(c, 4'000'000, 1.5);
    CHECK(c.kbps() == 36'000);  // no probing up within 2 s of the measure
}

KT_MAIN()
