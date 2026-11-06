#include <algorithm>
#include <numeric>
#include <random>

#include "kage/protocol/video_fragment.hpp"
#include "test_main.hpp"

using namespace kage::protocol;

static std::vector<uint8_t> makeFrame(size_t n, uint8_t seed) {
    std::vector<uint8_t> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = static_cast<uint8_t>(i * 31 + seed);
    return f;
}

static std::vector<std::vector<uint8_t>> split(uint32_t idx, const std::vector<uint8_t>& f, uint8_t flags = 0) {
    std::vector<std::vector<uint8_t>> out;
    fragmentFrame(idx, 1000 + idx, flags, f, [&](std::span<const uint8_t> p) { out.emplace_back(p.begin(), p.end()); });
    return out;
}

TEST(fragments_fit_in_datagram) {
    auto frags = split(1, makeFrame(100000, 1));
    CHECK(frags.size() == (100000 + kMaxVideoPayload - 1) / kMaxVideoPayload);
    for (auto& f : frags) CHECK(f.size() + PacketHeader::kSize + 16 <= kMaxDatagram);
}

TEST(reassemble_in_order_and_shuffled) {
    auto frame = makeFrame(50000, 7);
    auto frags = split(10, frame, kFrameKeyframe);
    std::mt19937 rng(42);
    std::shuffle(frags.begin(), frags.end(), rng);

    FrameReassembler r;
    std::optional<CompletedFrame> done;
    for (size_t i = 0; i < frags.size(); ++i) {
        auto res = r.push(frags[i]);
        if (i + 1 < frags.size()) CHECK(!res);
        else done = std::move(res);
    }
    REQUIRE(done.has_value());
    CHECK(done->frameIndex == 10 && done->poseTimeUs == 1010 && (done->flags & kFrameKeyframe));
    CHECK(done->data == frame);
    CHECK(r.stats().framesCompleted == 1);
}

TEST(duplicates_ignored) {
    auto frags = split(1, makeFrame(5000, 1));
    FrameReassembler r;
    r.push(frags[0]);
    r.push(frags[0]);
    CHECK(r.stats().fragmentsDuplicate == 1);
    for (size_t i = 1; i < frags.size(); ++i) r.push(frags[i]);
    CHECK(r.stats().framesCompleted == 1);
}

TEST(newer_frame_completion_drops_older_incomplete) {
    auto a = split(1, makeFrame(5000, 1));
    auto b = split(2, makeFrame(5000, 2));
    FrameReassembler r;
    r.push(a[0]);  // frame 1 incomplete
    std::optional<CompletedFrame> got;
    for (auto& f : b) if (auto x = r.push(f)) got = std::move(x);
    REQUIRE(got.has_value());
    CHECK(got->frameIndex == 2);
    CHECK(r.stats().framesDropped == 1);
    CHECK(r.keyframeNeeded());

    // Late fragments of frame 1 are now stale and never produce a frame.
    for (size_t i = 1; i < a.size(); ++i) CHECK(!r.push(a[i]));
    CHECK(r.stats().fragmentsStale == a.size() - 1);

    // A complete keyframe clears the request.
    for (auto& f : split(3, makeFrame(3000, 3), kFrameKeyframe)) r.push(f);
    CHECK(!r.keyframeNeeded());
}

TEST(rejects_inconsistent_and_out_of_bounds) {
    FrameReassembler r;
    auto frags = split(1, makeFrame(5000, 1));
    auto bad = frags[1];
    bad[4] ^= 0x01;  // change frameSize of 2nd fragment
    r.push(frags[0]);
    CHECK(!r.push(bad));
    CHECK(r.stats().fragmentsRejected == 1);

    // Offset beyond frame size.
    auto oob = frags[0];
    oob[8] = 0xFF;
    oob[9] = 0xFF;
    oob[10] = 0xFF;
    oob[11] = 0x00;
    FrameReassembler r2;
    CHECK(!r2.push(oob));
    CHECK(r2.stats().fragmentsRejected == 1);

    // Frame size above the cap.
    auto big = frags[0];
    big[7] = 0x7F;
    FrameReassembler r3;
    CHECK(!r3.push(big));

    CHECK(!r3.push(std::vector<uint8_t>(5, 0)));  // truncated header
}

TEST(bounded_inflight_under_heavy_loss) {
    FrameReassembler r;
    for (uint32_t i = 0; i < 1000; ++i) {
        auto frags = split(i, makeFrame(4000, static_cast<uint8_t>(i)));
        r.push(frags[0]);  // only the first fragment of each frame arrives
    }
    CHECK(r.stats().framesCompleted == 0);
    CHECK(r.stats().framesDropped >= 1000 - FrameReassembler::kMaxInFlight);
}

TEST(frame_index_wraparound) {
    FrameReassembler r;
    for (auto& f : split(0xFFFFFFFEu, makeFrame(2000, 1))) r.push(f);
    for (auto& f : split(0xFFFFFFFFu, makeFrame(2000, 2))) r.push(f);
    std::optional<CompletedFrame> got;
    for (auto& f : split(0u, makeFrame(2000, 3))) if (auto x = r.push(f)) got = std::move(x);
    REQUIRE(got.has_value());
    CHECK(got->frameIndex == 0);
    CHECK(r.stats().framesCompleted == 3);
}

KT_MAIN()
