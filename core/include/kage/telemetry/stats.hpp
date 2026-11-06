// KAGE VR Stream - lightweight streaming statistics for telemetry.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kage::telemetry {

// Fixed-capacity window of samples (ring buffer, no allocation after construction).
class RollingStats {
public:
    explicit RollingStats(size_t capacity = 256) : buf_(capacity ? capacity : 1) {}

    void add(double v) {
        buf_[head_] = v;
        head_ = (head_ + 1) % buf_.size();
        if (count_ < buf_.size()) ++count_;
    }
    size_t count() const { return count_; }
    bool empty() const { return count_ == 0; }

    double mean() const {
        if (!count_) return 0;
        double s = 0;
        for (size_t i = 0; i < count_; ++i) s += buf_[i];
        return s / static_cast<double>(count_);
    }
    double min() const { return count_ ? *std::min_element(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(count_)) : 0; }
    double max() const { return count_ ? *std::max_element(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(count_)) : 0; }
    double stddev() const {
        if (count_ < 2) return 0;
        const double m = mean();
        double s = 0;
        for (size_t i = 0; i < count_; ++i) s += (buf_[i] - m) * (buf_[i] - m);
        return std::sqrt(s / static_cast<double>(count_ - 1));
    }
    // p in [0,100], nearest-rank.
    double percentile(double p) const {
        if (!count_) return 0;
        std::vector<double> v(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(count_));
        std::sort(v.begin(), v.end());
        const double rank = std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(v.size() - 1);
        return v[static_cast<size_t>(std::lround(rank))];
    }

private:
    std::vector<double> buf_;
    size_t head_ = 0;
    size_t count_ = 0;
};

// Interarrival jitter estimator from RFC 3550 section 6.4.1.
// transit = arrival - send (any consistent clock pair); J += (|D| - J) / 16.
class JitterEstimator {
public:
    void add(int64_t sendUs, int64_t arrivalUs) {
        const int64_t transit = arrivalUs - sendUs;
        if (has_) {
            const double d = std::fabs(static_cast<double>(transit - lastTransit_));
            jitter_ += (d - jitter_) / 16.0;
        }
        lastTransit_ = transit;
        has_ = true;
    }
    double jitterUs() const { return jitter_; }

private:
    bool has_ = false;
    int64_t lastTransit_ = 0;
    double jitter_ = 0;
};

// Counts losses from a 32-bit sequence stream (tolerates reordering & wrap).
class LossCounter {
public:
    void add(uint32_t seq) {
        ++received_;
        if (!has_) {
            has_ = true;
            highest_ = seq;
            first_ = seq;
            return;
        }
        if (static_cast<int32_t>(seq - highest_) > 0) highest_ = seq;
    }
    uint64_t received() const { return received_; }
    uint64_t expected() const { return has_ ? static_cast<uint64_t>(highest_ - first_) + 1 : 0; }
    uint64_t lost() const { return expected() > received_ ? expected() - received_ : 0; }
    double lossRatio() const { return expected() ? static_cast<double>(lost()) / static_cast<double>(expected()) : 0; }

private:
    bool has_ = false;
    uint32_t first_ = 0;
    uint32_t highest_ = 0;
    uint64_t received_ = 0;
};

}  // namespace kage::telemetry
