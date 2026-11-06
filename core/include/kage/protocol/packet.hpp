// KAGE VR Stream - datagram header shared by every channel.
//
// Layout (24 bytes, little-endian):
//   u32 magic 'KAGE' | u8 version | u8 channel | u8 flags | u8 reserved(0)
//   u32 sessionId    | u32 sequence (per channel, wraps) | u64 sendTimeUs
//
// From Phase 2 on, the header is the AEAD "associated data" and the payload is
// encrypted; the header itself is authenticated but stays readable so packets
// can be routed and dropped cheaply before decryption.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kage::protocol {

inline constexpr uint32_t kMagic = 0x4547414B;  // bytes "KAGE" on the wire
inline constexpr uint8_t kProtocolVersion = 1;

// Conservative UDP payload budget: 1500 MTU - 20 IPv4 - 8 UDP - margin for
// tunnels/VPN/802.11 encapsulation quirks. Avoids IP fragmentation.
inline constexpr size_t kMaxDatagram = 1400;

enum class Channel : uint8_t {
    Discovery = 1,
    Control = 2,
    Tracking = 3,
    Video = 4,
    Audio = 5,
};

enum PacketFlags : uint8_t {
    kFlagEncrypted = 1u << 0,  // payload is AEAD ciphertext (Phase 2+)
};

struct PacketHeader {
    static constexpr size_t kSize = 24;

    uint8_t version = kProtocolVersion;
    Channel channel = Channel::Control;
    uint8_t flags = 0;
    uint32_t sessionId = 0;
    uint32_t sequence = 0;
    uint64_t sendTimeUs = 0;
};

enum class DecodeError {
    None,
    TooShort,
    TooLarge,
    BadMagic,
    BadVersion,
    BadChannel,
    BadReserved,
    Malformed,
};

const char* toString(DecodeError e);

// Appends header + payload to `out` (out is cleared first).
void encodePacket(const PacketHeader& h, std::span<const uint8_t> payload, std::vector<uint8_t>& out);

// Validates and splits a received datagram. `payload` aliases `datagram`.
DecodeError decodePacket(std::span<const uint8_t> datagram, PacketHeader& h,
                         std::span<const uint8_t>& payload);

// Sequence comparison tolerant to 32-bit wrap-around (RFC 1982 style).
inline bool seqNewer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

}  // namespace kage::protocol
