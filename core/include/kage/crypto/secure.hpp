// KAGE VR Stream - pairing and stream encryption, built on Monocypher
// (third_party/monocypher, audited X25519 / BLAKE2b / XChaCha20-Poly1305).
//
// Identity: each side has a long-term X25519 key pair stored locally.
// Pairing: the user checks that the 6-digit code shown in the headset equals
//          the one shown on the PC (code = BLAKE2b(client key || host key)),
//          which defeats a man in the middle; the PC then trusts the headset key.
// Session: Noise-KK-like exchange mixing static and ephemeral keys
//          (mutual authentication + forward secrecy), one key per direction.
// Packets: XChaCha20-Poly1305, packet header as associated data, explicit
//          64-bit counter as nonce, sliding-window replay protection.
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kage/protocol/packet.hpp"

namespace kage::crypto {

using Key = std::array<uint8_t, 32>;

bool randomBytes(uint8_t* out, size_t n);
void wipe(void* p, size_t n);

struct KeyPair {
    Key secret{}, pub{};
    static std::optional<KeyPair> generate();
    static KeyPair fromSecret(const Key& secret);
    void wipe();
};

// 32-byte secret file, created on first use (user-only location expected).
std::optional<KeyPair> loadOrCreateKeyPair(const std::filesystem::path& file);

uint32_t pairingCode(const Key& clientKey, const Key& hostKey);  // 0..999999
std::string toHex(const Key& k);
std::optional<Key> fromHex(std::string_view hex);

// Trusted peer keys, one "hex name" per line.
bool isTrusted(const std::filesystem::path& file, const Key& k);
bool addTrusted(const std::filesystem::path& file, const Key& k, std::string_view name);
bool removeTrusted(const std::filesystem::path& file, const Key& k);

struct SessionKeys {
    Key send{}, recv{};
    std::array<uint8_t, 16> confirm{};
};

// Fails (nullopt) on degenerate (low-order) public keys.
std::optional<SessionKeys> deriveClient(const KeyPair& clientStatic, const KeyPair& clientEph, const Key& hostStatic,
                                        const Key& hostEph);
std::optional<SessionKeys> deriveHost(const KeyPair& hostStatic, const KeyPair& hostEph, const Key& clientStatic,
                                      const Key& clientEph);
bool equal16(const std::array<uint8_t, 16>& a, const std::array<uint8_t, 16>& b);  // constant time

class SecureSession {
public:
    static constexpr size_t kOverhead = 8 + 16;  // counter + MAC

    void init(const SessionKeys& k);
    void reset();
    bool active() const { return active_; }

    // Builds a full datagram: header (flag encrypted) | u64 counter | ciphertext | MAC.
    void seal(protocol::PacketHeader h, std::span<const uint8_t> plain, std::vector<uint8_t>& out);
    // Decrypts a datagram whose header has already been decoded; rejects forgeries and replays.
    bool open(std::span<const uint8_t> datagram, const protocol::PacketHeader& h, std::span<const uint8_t> body,
              std::vector<uint8_t>& plain);

private:
    static constexpr uint64_t kWindow = 1024;
    Key send_{}, recv_{};
    uint64_t sendCounter_ = 0;
    uint64_t recvHigh_ = 0;
    bool recvAny_ = false;
    std::bitset<kWindow> seen_;
    bool active_ = false;
};

}  // namespace kage::crypto
