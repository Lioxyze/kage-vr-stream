#include "kage/crypto/secure.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

#include "monocypher.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <stdlib.h>  // arc4random_buf (bionic)
#endif

namespace kage::crypto {

using protocol::PacketHeader;

bool randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    arc4random_buf(out, n);
    return true;
#endif
}

void wipe(void* p, size_t n) { crypto_wipe(p, n); }

std::optional<KeyPair> KeyPair::generate() {
    Key s;
    if (!randomBytes(s.data(), s.size())) return std::nullopt;
    auto kp = fromSecret(s);
    crypto::wipe(s.data(), s.size());
    return kp;
}

KeyPair KeyPair::fromSecret(const Key& secret) {
    KeyPair kp;
    kp.secret = secret;
    crypto_x25519_public_key(kp.pub.data(), kp.secret.data());
    return kp;
}

void KeyPair::wipe() { crypto::wipe(secret.data(), secret.size()); }

std::optional<KeyPair> loadOrCreateKeyPair(const std::filesystem::path& file) {
    {
        std::ifstream in(file, std::ios::binary);
        Key s{};
        if (in.read(reinterpret_cast<char*>(s.data()), s.size()) && in.gcount() == 32) {
            auto kp = KeyPair::fromSecret(s);
            crypto::wipe(s.data(), s.size());
            return kp;
        }
    }
    auto kp = KeyPair::generate();
    if (!kp) return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(kp->secret.data()), kp->secret.size());
    if (!out) return std::nullopt;
    return kp;
}

uint32_t pairingCode(const Key& clientKey, const Key& hostKey) {
    uint8_t buf[64], h[8];
    std::memcpy(buf, clientKey.data(), 32);
    std::memcpy(buf + 32, hostKey.data(), 32);
    crypto_blake2b(h, sizeof h, buf, sizeof buf);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(h[i]) << (8 * i);
    return static_cast<uint32_t>(v % 1'000'000);
}

std::string toHex(const Key& k) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : k) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

std::optional<Key> fromHex(std::string_view hex) {
    if (hex.size() != 64) return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Key k;
    for (size_t i = 0; i < 32; ++i) {
        const int a = nib(hex[2 * i]), b = nib(hex[2 * i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        k[i] = static_cast<uint8_t>(a << 4 | b);
    }
    return k;
}

namespace {
std::vector<std::string> readLines(const std::filesystem::path& file) {
    std::vector<std::string> lines;
    std::ifstream in(file);
    for (std::string l; std::getline(in, l);)
        if (!l.empty()) lines.push_back(l);
    return lines;
}
bool lineHasKey(const std::string& l, const Key& k) { return l.size() >= 64 && fromHex(l.substr(0, 64)) == k; }
}  // namespace

bool isTrusted(const std::filesystem::path& file, const Key& k) {
    for (const auto& l : readLines(file))
        if (lineHasKey(l, k)) return true;
    return false;
}

bool addTrusted(const std::filesystem::path& file, const Key& k, std::string_view name) {
    if (isTrusted(file, k)) return true;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::app);
    std::string clean(name.substr(0, 64));
    for (char& c : clean)
        if (c == '\n' || c == '\r') c = ' ';
    out << toHex(k) << ' ' << clean << '\n';
    return static_cast<bool>(out);
}

bool removeTrusted(const std::filesystem::path& file, const Key& k) {
    auto lines = readLines(file);
    std::ofstream out(file, std::ios::trunc);
    for (const auto& l : lines)
        if (!lineHasKey(l, k)) out << l << '\n';
    return static_cast<bool>(out);
}

namespace {
bool dh(uint8_t out[32], const Key& secret, const Key& pub) {
    crypto_x25519(out, secret.data(), pub.data());
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= out[i];
    return acc != 0;  // all-zero = low-order point: reject
}

// Transcript order is fixed (client static, client eph, host static, host eph),
// DH results in the order seen by the client.
std::optional<SessionKeys> finish(const Key& cs, const Key& ce, const Key& hs, const Key& he, uint8_t dhs[4][32],
                                  bool client) {
    static const char kLabel[] = "KAGE-VR-STREAM KK v1";
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(kLabel), sizeof kLabel - 1);
    for (const Key* k : {&cs, &ce, &hs, &he}) crypto_blake2b_update(&ctx, k->data(), 32);
    for (int i = 0; i < 4; ++i) crypto_blake2b_update(&ctx, dhs[i], 32);
    uint8_t okm[64];
    crypto_blake2b_final(&ctx, okm);
    crypto_wipe(dhs, 4 * 32);
    Key c2s, s2c;
    uint8_t conf[32];
    const uint8_t l1 = 1, l2 = 2, l3 = 3;
    crypto_blake2b_keyed(c2s.data(), 32, okm, 64, &l1, 1);
    crypto_blake2b_keyed(s2c.data(), 32, okm, 64, &l2, 1);
    crypto_blake2b_keyed(conf, 32, okm, 64, &l3, 1);
    crypto_wipe(okm, sizeof okm);
    SessionKeys k;
    k.send = client ? c2s : s2c;
    k.recv = client ? s2c : c2s;
    std::memcpy(k.confirm.data(), conf, 16);
    crypto_wipe(c2s.data(), 32);
    crypto_wipe(s2c.data(), 32);
    return k;
}
}  // namespace

std::optional<SessionKeys> deriveClient(const KeyPair& cs, const KeyPair& ce, const Key& hs, const Key& he) {
    uint8_t d[4][32];
    if (!dh(d[0], ce.secret, he) || !dh(d[1], cs.secret, hs) || !dh(d[2], ce.secret, hs) || !dh(d[3], cs.secret, he)) {
        crypto_wipe(d, sizeof d);
        return std::nullopt;
    }
    return finish(cs.pub, ce.pub, hs, he, d, true);
}

std::optional<SessionKeys> deriveHost(const KeyPair& hs, const KeyPair& he, const Key& cs, const Key& ce) {
    uint8_t d[4][32];
    if (!dh(d[0], he.secret, ce) || !dh(d[1], hs.secret, cs) || !dh(d[2], hs.secret, ce) || !dh(d[3], he.secret, cs)) {
        crypto_wipe(d, sizeof d);
        return std::nullopt;
    }
    return finish(cs, ce, hs.pub, he.pub, d, false);
}

bool equal16(const std::array<uint8_t, 16>& a, const std::array<uint8_t, 16>& b) {
    return crypto_verify16(a.data(), b.data()) == 0;
}

// ---- per-packet AEAD ----------------------------------------------------------
void SecureSession::init(const SessionKeys& k) {
    send_ = k.send;
    recv_ = k.recv;
    sendCounter_ = 0;
    recvHigh_ = 0;
    recvAny_ = false;
    seen_.reset();
    active_ = true;
}

void SecureSession::reset() {
    crypto_wipe(send_.data(), 32);
    crypto_wipe(recv_.data(), 32);
    active_ = false;
}

namespace {
void nonceFor(uint64_t counter, uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<uint8_t>(counter >> (8 * i));
}
}  // namespace

void SecureSession::seal(PacketHeader h, std::span<const uint8_t> plain, std::vector<uint8_t>& out) {
    h.flags |= protocol::kFlagEncrypted;
    const uint64_t counter = sendCounter_++;
    // Header first (it is the associated data), then counter, ciphertext, MAC.
    protocol::encodePacket(h, {}, out);
    const size_t hdr = out.size();
    out.resize(hdr + 8 + plain.size() + 16);
    for (int i = 0; i < 8; ++i) out[hdr + i] = static_cast<uint8_t>(counter >> (8 * i));
    uint8_t nonce[24];
    nonceFor(counter, nonce);
    crypto_aead_lock(out.data() + hdr + 8, out.data() + hdr + 8 + plain.size(), send_.data(), nonce, out.data(), hdr,
                     plain.data(), plain.size());
}

bool SecureSession::open(std::span<const uint8_t> datagram, const PacketHeader& h, std::span<const uint8_t> body,
                         std::vector<uint8_t>& plain) {
    if (!active_ || !(h.flags & protocol::kFlagEncrypted) || body.size() < kOverhead) return false;
    uint64_t counter = 0;
    for (int i = 0; i < 8; ++i) counter |= static_cast<uint64_t>(body[i]) << (8 * i);
    if (recvAny_ && counter + kWindow <= recvHigh_) return false;              // too old
    if (recvAny_ && counter <= recvHigh_ && seen_[counter % kWindow]) return false;  // replay
    const size_t textSize = body.size() - kOverhead;
    plain.resize(textSize);
    uint8_t nonce[24];
    nonceFor(counter, nonce);
    const size_t adSize = static_cast<size_t>(body.data() - datagram.data());
    if (crypto_aead_unlock(plain.data(), body.data() + 8 + textSize, recv_.data(), nonce, datagram.data(), adSize,
                           body.data() + 8, textSize) != 0)
        return false;
    // Authentic: update the replay window only now, so forgeries cannot move it.
    if (!recvAny_ || counter > recvHigh_) {
        const uint64_t shift = recvAny_ ? counter - recvHigh_ : kWindow;
        if (shift >= kWindow) seen_.reset();
        else
            for (uint64_t i = recvHigh_ + 1; i <= counter; ++i) seen_[i % kWindow] = false;
        recvHigh_ = counter;
        recvAny_ = true;
    }
    seen_[counter % kWindow] = true;
    return true;
}

}  // namespace kage::crypto
