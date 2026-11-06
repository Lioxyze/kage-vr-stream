#pragma once

#include <chrono>
#include <cstdint>

namespace kage {

// Monotonic microseconds since an arbitrary epoch (process-local meaning).
inline uint64_t monotonicMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace kage
