#include "kage/sync/clock_sync.hpp"

namespace kage::sync {

std::optional<ClockSample> ClockSync::addSample(uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3) {
    if (t3 < t0 || t2 < t1) return std::nullopt;
    const int64_t total = static_cast<int64_t>(t3 - t0);
    const int64_t hostHold = static_cast<int64_t>(t2 - t1);
    if (hostHold > total) return std::nullopt;  // impossible: host held the packet longer than the round trip

    ClockSample s;
    s.rttUs = total - hostHold;
    // Signed arithmetic on differences; clocks may be arbitrarily far apart.
    const int64_t a = static_cast<int64_t>(t1) - static_cast<int64_t>(t0);
    const int64_t b = static_cast<int64_t>(t2) - static_cast<int64_t>(t3);
    s.offsetUs = a / 2 + b / 2 + ((a % 2) + (b % 2)) / 2;

    samples_.push_back(s);
    while (samples_.size() > window_) samples_.pop_front();

    best_ = samples_.front();
    for (const auto& x : samples_)
        if (x.rttUs < best_.rttUs) best_ = x;
    return s;
}

}  // namespace kage::sync
