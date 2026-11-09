// Deterministic AIMD bitrate control from the headset's reports (every 250 ms, or 1 s
// on old headsets). Pure logic, no clock or I/O: unit-tested and replayed on real logs.
//
// Congestion = lost frames, frames that never arrived, or one-way transit rising above the
// best seen (mild delay for 2 s, or a big delay twice in a row). Back off 25 %, then probe
// up: +10 %/s, or +30 %/s while below the last rate that was stable (quick recovery after
// an isolated Wi-Fi hiccup).
#pragma once

#include <algorithm>
#include <cstdint>

namespace kage::driver {

class BitrateController {
public:
    static constexpr uint32_t kMinKbps = 10'000;
    // Delay alone (no lost frame) never pushes below this: on a jittery Wi-Fi the delay
    // stays high even at low rates, and cutting further only degrades the image.
    static constexpr uint32_t kDelayFloorKbps = 50'000;

    struct Report {
        uint64_t nowUs = 0;
        uint32_t framesReceived = 0, framesDropped = 0;
        uint32_t framesSent = 0;  // encoded by the PC over the same interval
        uint32_t networkUsAvg = 0;
    };

    explicit BitrateController(uint32_t maxKbps = 150'000, uint32_t startKbps = 60'000)
        : maxKbps_(maxKbps), kbps_(std::min(startKbps, maxKbps)) {}

    uint32_t kbps() const { return kbps_; }
    uint32_t bestNetUs() const { return bestNetUs_; }
    bool lastCongested() const { return congested_; }

    // Rate set from outside (session-start probe): no probing up for 2 s.
    void set(uint32_t kbps, uint64_t nowUs) {
        kbps_ = std::min(kbps, maxKbps_);
        holdUntilUs_ = nowUs + 2'000'000;
    }

    // Returns the new rate (unchanged if no action).
    uint32_t update(const Report& r) {
        const uint64_t now = r.nowUs;
        const uint64_t dt = lastUs_ ? std::clamp<uint64_t>(now - lastUs_, 100'000, 2'000'000) : 1'000'000;
        lastUs_ = now;
        if (!firstUs_) firstUs_ = now;
        const double dts = dt / 1e6;
        const uint32_t net = r.networkUsAvg;
        // Baseline = lowest transit seen (0 = no frame in that report: ignored).
        if (net && (bestNetUs_ == 0 || net < bestNetUs_)) bestNetUs_ = net;
        if (bestNetUs_) bestNetUs_ += static_cast<uint32_t>(50 * dts);  // slowly forget an old optimum
        delayUs_ = (net && bestNetUs_ && net > bestNetUs_ + 10000) ? delayUs_ + dt : 0;
        // A big queueing delay (> 25 ms over the best) is acted on when seen twice in a row
        // (~0.5 s): a lone spike is Wi-Fi noise, a queue that persists only drains if we send less.
        const bool big = net && bestNetUs_ && net > bestNetUs_ + 25000;
        bigCount_ = big ? bigCount_ + 1 : 0;
        // Frames that never arrived: a running balance, so frames still in flight at a report
        // boundary (counted in the next report) do not look lost. Decays when calm.
        missing_ = std::max(0.0, missing_ + static_cast<double>(r.framesSent) - r.framesReceived);
        missing_ = std::max(0.0, missing_ - 2.0 * dts);  // tolerance: 2 frames/s of jitter
        const bool startup = now - firstUs_ < 5'000'000;
        if (startup) missing_ = 0;  // SteamVR start-up stalls and the probe hold are not losses
        const bool missing = missing_ > std::max(3.0, r.framesSent * 0.25);
        const bool loss = r.framesDropped > 0 || missing;
        congested_ = !startup && (loss || delayUs_ >= 2'000'000 || bigCount_ >= 2);
        if (startup) return kbps_;
        if (congested_) {
            if (now - lastCutUs_ >= 400'000) {  // at most one cut per 400 ms: let it show
                // The rate held calmly before this cut is the target of the quick recovery.
                if (calmUs_ >= 2'000'000) lastGoodKbps_ = kbps_;
                const uint32_t floor = loss ? kMinKbps : std::min(kDelayFloorKbps, kbps_);
                kbps_ = std::max<uint32_t>(floor, kbps_ * 3 / 4);
                lastCutUs_ = now;
                if (missing) missing_ = 0;  // acted on: start a fresh balance
            }
            holdUntilUs_ = now + 1'000'000;  // hold 1 s before probing again
            calmUs_ = 0;
        } else {
            calmUs_ += dt;
            if (now >= holdUntilUs_) {
                const double step = kbps_ < lastGoodKbps_ * 9 / 10 ? 0.30 : 0.10;
                uint32_t up = kbps_ + static_cast<uint32_t>(kbps_ * step * dts);
                if (step > 0.10) up = std::min(up, lastGoodKbps_);  // quick part stops at the old good rate
                kbps_ = std::min(maxKbps_, up);
            }
        }
        return kbps_;
    }

private:
    uint32_t maxKbps_, kbps_;
    uint32_t bestNetUs_ = 0, bigCount_ = 0, lastGoodKbps_ = 0;
    double missing_ = 0;
    uint64_t lastUs_ = 0, firstUs_ = 0, delayUs_ = 0, lastCutUs_ = 0, holdUntilUs_ = 0, calmUs_ = 0;
    bool congested_ = false;
};

}  // namespace kage::driver
