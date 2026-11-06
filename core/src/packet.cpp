#include "kage/protocol/packet.hpp"

#include "kage/protocol/wire.hpp"

namespace kage::protocol {

const char* toString(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::TooShort: return "too short";
        case DecodeError::TooLarge: return "too large";
        case DecodeError::BadMagic: return "bad magic";
        case DecodeError::BadVersion: return "unsupported protocol version";
        case DecodeError::BadChannel: return "unknown channel";
        case DecodeError::BadReserved: return "reserved field not zero";
        case DecodeError::Malformed: return "malformed";
    }
    return "?";
}

void encodePacket(const PacketHeader& h, std::span<const uint8_t> payload, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(PacketHeader::kSize + payload.size());
    wire::Writer w(out);
    w.u32(kMagic);
    w.u8(h.version);
    w.u8(static_cast<uint8_t>(h.channel));
    w.u8(h.flags);
    w.u8(0);
    w.u32(h.sessionId);
    w.u32(h.sequence);
    w.u64(h.sendTimeUs);
    w.bytes(payload);
}

DecodeError decodePacket(std::span<const uint8_t> datagram, PacketHeader& h,
                         std::span<const uint8_t>& payload) {
    if (datagram.size() < PacketHeader::kSize) return DecodeError::TooShort;
    if (datagram.size() > kMaxDatagram) return DecodeError::TooLarge;

    wire::Reader r(datagram);
    uint32_t magic = 0;
    uint8_t channel = 0, reserved = 0;
    r.u32(magic);
    r.u8(h.version);
    r.u8(channel);
    r.u8(h.flags);
    r.u8(reserved);
    r.u32(h.sessionId);
    r.u32(h.sequence);
    r.u64(h.sendTimeUs);
    if (!r.ok()) return DecodeError::Malformed;

    if (magic != kMagic) return DecodeError::BadMagic;
    if (h.version != kProtocolVersion) return DecodeError::BadVersion;
    if (channel < static_cast<uint8_t>(Channel::Discovery) || channel > static_cast<uint8_t>(Channel::Audio))
        return DecodeError::BadChannel;
    if (reserved != 0) return DecodeError::BadReserved;
    if ((h.flags & ~kFlagEncrypted) != 0) return DecodeError::Malformed;

    h.channel = static_cast<Channel>(channel);
    payload = r.rest();
    return DecodeError::None;
}

}  // namespace kage::protocol
