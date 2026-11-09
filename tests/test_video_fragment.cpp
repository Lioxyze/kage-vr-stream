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

static std::vector<std::vector<uint8_t>> splitFec(uint32_t idx, const std::vector<uint8_t>& f, unsigned group) {
    std::vector<std::vector<uint8_t>> out;
    fragmentFrame(idx, 1000 + idx, 0, f, [&](std::span<const uint8_t> p) { out.emplace_back(p.begin(), p.end()); },
                  kMaxVideoPayload, group);
    return out;
}

TEST(fec_recovers_one_loss_per_group_including_last_short_fragment) {
    auto frame = makeFrame(100000, 3);  // 76 data fragments, last one short
    const size_t data = (frame.size() + kMaxVideoPayload - 1) / kMaxVideoPayload;
    auto frags = splitFec(5, frame, 8);
    const size_t groups = (data + 7) / 8;
    CHECK(frags.size() == data + groups);
    for (auto& f : frags) CHECK(f.size() + PacketHeader::kSize + 24 <= kMaxDatagram);
    // A burst of `groups` consecutive losses hits each interleaved group once; include the last one.
    std::vector<bool> lost(frags.size(), false);
    for (size_t i = data - groups; i < data; ++i) lost[i] = true;
    FrameReassembler r;
    std::optional<CompletedFrame> done;
    for (size_t i = 0; i < frags.size(); ++i)
        if (!lost[i])
            if (auto d = r.push(frags[i])) done = std::move(d);
    CHECK(done.has_value());
    CHECK(done && done->data == frame);
    CHECK(r.stats().fragmentsRecovered == groups);
    CHECK(!r.keyframeNeeded());
}

TEST(fec_parity_before_data_and_two_losses_in_group) {
    auto frame = makeFrame(30000, 9);
    auto frags = splitFec(6, frame, 4);
    const size_t data = (frame.size() + kMaxVideoPayload - 1) / kMaxVideoPayload;
    const size_t groups = frags.size() - data;
    {  // parity first, then data with one loss: rebuilt as soon as the rest arrives
        FrameReassembler r;
        for (size_t i = data; i < frags.size(); ++i) CHECK(!r.push(frags[i]));
        std::optional<CompletedFrame> done;
        for (size_t i = 1; i < data; ++i)
            if (auto d = r.push(frags[i])) done = std::move(d);
        CHECK(done && done->data == frame);
    }
    {  // two losses in the same group: not recoverable, frame stays incomplete
        FrameReassembler r;
        std::optional<CompletedFrame> done;
        for (size_t i = 0; i < frags.size(); ++i)
            if (i != 0 && i != groups)
                if (auto d = r.push(frags[i])) done = std::move(d);
        CHECK(!done);
    }
}

TEST(fec_corrupt_parity_rejected) {
    auto frame = makeFrame(20000, 1);
    auto frags = splitFec(7, frame, 4);
    auto bad = frags.back();
    bad.resize(bad.size() - 1);  // wrong fragment size
    FrameReassembler r;
    CHECK(!r.push(bad));  // arrives first: accepted, but its size disagrees with the data
    const size_t data = (frame.size() + kMaxVideoPayload - 1) / kMaxVideoPayload;
    std::optional<CompletedFrame> done;
    for (size_t i = 1; i < data; ++i)
        if (auto d = r.push(frags[i])) done = std::move(d);
    CHECK(!done);  // never rebuilt from a mismatched parity (no corrupt frame)
    CHECK(r.stats().fragmentsRecovered == 0);
}

TEST(slice_units_accepted_and_ordered) {
    // Frame 3 in two slice units (ids 3*kMaxSlices + 0/1); the last one carries kFrameSliceLast.
    auto s0 = makeFrame(9000, 1), s1 = makeFrame(5000, 2);
    std::vector<std::vector<uint8_t>> a, b;
    fragmentFrame(3 * kMaxSlices, 1, kFrameSliced | kFrameKeyframe, s0,
                  [&](std::span<const uint8_t> p) { a.emplace_back(p.begin(), p.end()); }, kMaxVideoPayload, 8);
    fragmentFrame(3 * kMaxSlices + 1, 1, kFrameSliced | kFrameSliceLast | kFrameKeyframe, s1,
                  [&](std::span<const uint8_t> p) { b.emplace_back(p.begin(), p.end()); }, kMaxVideoPayload, 8);
    FrameReassembler r;
    std::vector<CompletedFrame> out;
    for (auto& p : a) if (auto d = r.push(p)) out.push_back(std::move(*d));
    for (auto& p : b) if (auto d = r.push(p)) out.push_back(std::move(*d));
    CHECK(out.size() == 2);
    CHECK(out.size() == 2 && out[0].data == s0 && out[1].data == s1);
    CHECK(out.size() == 2 && (out[1].flags & kFrameSliceLast) && (out[0].flags & kFrameSliced));
    CHECK(r.stats().fragmentsRejected == 0);
}
