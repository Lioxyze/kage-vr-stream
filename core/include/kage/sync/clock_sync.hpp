// KAGE VR Stream - host/client clock offset estimation.
//
// NTP-style four-timestamp exchange:
//   t0 client send, t1 host receive, t2 host send, t3 client receive
//   rtt    = (t3 - t0) - (t2 - t1)
//   offset = ((t1 - t0) + (t2 - t3)) / 2       (host_clock = client_clock + offset)
// The estimate uses the sample with the lowest RTT inside a sliding window:
// queuing delay only ever adds latency, so the fastest exchange is the one
// least distorted by asymmetric queuing (classic min-filter).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace kage::sync {

struct ClockSample {
    int64_t rttUs = 0;
    int64_t offsetUs = 0;
};

class ClockSync {
public:
    explicit ClockSync(size_t window = 32) : window_(window ? window : 1) {}

    // Returns the computed sample, or nullopt if timestamps are inconsistent.
    std::optional<ClockSample> addSample(uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3);

    bool hasEstimate() const { return !samples_.empty(); }
    int64_t offsetUs() const { return best_.offsetUs; }
    int64_t bestRttUs() const { return best_.rttUs; }
    int64_t lastRttUs() const { return samples_.empty() ? 0 : samples_.back().rttUs; }
    size_t sampleCount() const { return samples_.size(); }

    uint64_t clientToHost(uint64_t clientUs) const { return static_cast<uint64_t>(static_cast<int64_t>(clientUs) + best_.offsetUs); }
    uint64_t hostToClient(uint64_t hostUs) const { return static_cast<uint64_t>(static_cast<int64_t>(hostUs) - best_.offsetUs); }

private:
    size_t window_;
    std::deque<ClockSample> samples_;
    ClockSample best_;
};

}  // namespace kage::sync
