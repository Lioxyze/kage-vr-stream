// KAGE VR Stream - bounds-checked little-endian serialization primitives.
//
// All multi-byte integers on the wire are little-endian, encoded with explicit
// shifts so the result does not depend on host endianness or struct packing.
// The Reader is "sticky": the first failure sets ok() to false and every later
// read fails too, so decoders can read a whole message and check once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kage::wire {

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}

    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void i64(int64_t v) { put(static_cast<uint64_t>(v), 8); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    void bytes(std::span<const uint8_t> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    // Length-prefixed (u16) UTF-8 string. Longer strings are truncated to 65535 bytes.
    void str(std::string_view s) {
        const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        u16(static_cast<uint16_t>(n));
        out_.insert(out_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }
    size_t size() const { return out_.size(); }

private:
    void put(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> in) : in_(in) {}

    bool u8(uint8_t& v) {
        uint64_t t;
        if (!get(t, 1)) return false;
        v = static_cast<uint8_t>(t);
        return true;
    }
    bool u16(uint16_t& v) {
        uint64_t t;
        if (!get(t, 2)) return false;
        v = static_cast<uint16_t>(t);
        return true;
    }
    bool u32(uint32_t& v) {
        uint64_t t;
        if (!get(t, 4)) return false;
        v = static_cast<uint32_t>(t);
        return true;
    }
    bool u64(uint64_t& v) { return get(v, 8); }
    bool i64(int64_t& v) {
        uint64_t t;
        if (!get(t, 8)) return false;
        v = static_cast<int64_t>(t);
        return true;
    }
    bool f32(float& v) {
        uint32_t bits;
        if (!u32(bits)) return false;
        std::memcpy(&v, &bits, sizeof v);
        return true;
    }
    // Zero-copy view of the next n bytes.
    bool bytes(std::span<const uint8_t>& view, size_t n) {
        if (!require(n)) return false;
        view = in_.subspan(pos_, n);
        pos_ += n;
        return true;
    }
    bool str(std::string& s, size_t maxLen) {
        uint16_t n;
        if (!u16(n)) return false;
        if (n > maxLen) return fail();
        std::span<const uint8_t> v;
        if (!bytes(v, n)) return false;
        s.assign(reinterpret_cast<const char*>(v.data()), v.size());
        return true;
    }
    std::span<const uint8_t> rest() {
        auto r = in_.subspan(pos_);
        pos_ = in_.size();
        return r;
    }

    bool ok() const { return ok_; }
    size_t remaining() const { return in_.size() - pos_; }
    bool fail() {
        ok_ = false;
        return false;
    }

private:
    bool require(size_t n) {
        if (!ok_ || in_.size() - pos_ < n) return fail();
        return true;
    }
    bool get(uint64_t& v, int n) {
        if (!require(static_cast<size_t>(n))) return false;
        v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(in_[pos_ + i]) << (8 * i);
        pos_ += static_cast<size_t>(n);
        return true;
    }

    std::span<const uint8_t> in_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace kage::wire
