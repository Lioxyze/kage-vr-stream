#include <cmath>
#include <limits>
#include <random>

#include "kage/protocol/messages.hpp"
#include "kage/protocol/packet.hpp"
#include "kage/protocol/wire.hpp"
#include "test_main.hpp"

using namespace kage::protocol;

TEST(wire_roundtrip_little_endian) {
    std::vector<uint8_t> buf;
    kage::wire::Writer w(buf);
    w.u16(0x1234);
    w.u32(0xA1B2C3D4);
    w.u64(0x0102030405060708ull);
    w.f32(-1.5f);
    w.str("quest");
    CHECK(buf[0] == 0x34 && buf[1] == 0x12);  // explicit little-endian
    CHECK(buf[2] == 0xD4 && buf[5] == 0xA1);

    kage::wire::Reader r(buf);
    uint16_t a;
    uint32_t b;
    uint64_t c;
    float d;
    std::string s;
    r.u16(a);
    r.u32(b);
    r.u64(c);
    r.f32(d);
    r.str(s, 16);
    CHECK(r.ok());
    CHECK(a == 0x1234 && b == 0xA1B2C3D4 && c == 0x0102030405060708ull && d == -1.5f && s == "quest");
    CHECK(r.remaining() == 0);
}

TEST(wire_reader_is_sticky_on_underflow) {
    std::vector<uint8_t> buf{1, 2, 3};
    kage::wire::Reader r(buf);
    uint32_t v;
    CHECK(!r.u32(v));
    uint8_t b;
    CHECK(!r.u8(b));  // stays failed even though 3 bytes remain
    CHECK(!r.ok());
}

TEST(wire_string_length_limit) {
    std::vector<uint8_t> buf;
    kage::wire::Writer w(buf);
    w.str(std::string(100, 'x'));
    kage::wire::Reader r(buf);
    std::string s;
    CHECK(!r.str(s, 64));
}

TEST(packet_roundtrip) {
    PacketHeader h;
    h.channel = Channel::Tracking;
    h.sessionId = 0xDEADBEEF;
    h.sequence = 42;
    h.sendTimeUs = 123456789;
    std::vector<uint8_t> payload{9, 8, 7}, out;
    encodePacket(h, payload, out);
    CHECK(out.size() == PacketHeader::kSize + 3);
    CHECK(out[0] == 'K' && out[1] == 'A' && out[2] == 'G' && out[3] == 'E');

    PacketHeader d;
    std::span<const uint8_t> p;
    REQUIRE(decodePacket(out, d, p) == DecodeError::None);
    CHECK(d.channel == Channel::Tracking && d.sessionId == 0xDEADBEEF && d.sequence == 42 && d.sendTimeUs == 123456789);
    CHECK(p.size() == 3 && p[0] == 9);
}

TEST(packet_rejects_malformed) {
    PacketHeader h;
    std::vector<uint8_t> out;
    encodePacket(h, {}, out);
    PacketHeader d;
    std::span<const uint8_t> p;

    CHECK(decodePacket(std::span(out).first(10), d, p) == DecodeError::TooShort);
    auto bad = out;
    bad[0] ^= 0xFF;
    CHECK(decodePacket(bad, d, p) == DecodeError::BadMagic);
    bad = out;
    bad[4] = 99;
    CHECK(decodePacket(bad, d, p) == DecodeError::BadVersion);
    bad = out;
    bad[5] = 0;
    CHECK(decodePacket(bad, d, p) == DecodeError::BadChannel);
    bad[5] = 200;
    CHECK(decodePacket(bad, d, p) == DecodeError::BadChannel);
    bad = out;
    bad[7] = 1;
    CHECK(decodePacket(bad, d, p) == DecodeError::BadReserved);
    bad = out;
    bad[6] = 0x80;
    CHECK(decodePacket(bad, d, p) == DecodeError::Malformed);
    std::vector<uint8_t> huge(kMaxDatagram + 1, 0);
    CHECK(decodePacket(huge, d, p) == DecodeError::TooLarge);
}

TEST(sequence_wraparound) {
    CHECK(seqNewer(1, 0));
    CHECK(seqNewer(0, 0xFFFFFFFFu));
    CHECK(!seqNewer(0xFFFFFFFFu, 0));
    CHECK(!seqNewer(5, 5));
}

template <class T>
static T roundtrip(const T& m) {
    std::vector<uint8_t> buf;
    encodeMessage(m, buf);
    auto d = decodeMessage(buf);
    if (!d || !std::holds_alternative<T>(*d)) {
        ++kt::failures();
        std::fprintf(stderr, "  roundtrip failed for message type %d\n", static_cast<int>(typeOf(m)));
        return T{};
    }
    return std::get<T>(*d);
}

TEST(messages_roundtrip) {
    auto p = roundtrip(DiscoveryProbe{77, "Quest 3"});
    CHECK(p.clientNonce == 77 && p.clientName == "Quest 3");

    DiscoveryAnnounce a;
    a.clientNonce = 77;
    a.hostName = "DESKTOP";
    a.controlPort = 47801;
    a.hostKeyFingerprint = {1, 2, 3, 4, 5, 6, 7, 8};
    auto a2 = roundtrip(a);
    CHECK(a2.hostName == "DESKTOP" && a2.controlPort == 47801 && a2.hostKeyFingerprint[7] == 8);

    auto pg = roundtrip(Pong{5, 100, 200, 250});
    CHECK(pg.pingId == 5 && pg.clientSendUs == 100 && pg.hostRecvUs == 200 && pg.hostSendUs == 250);

    auto dc = roundtrip(Disconnect{DisconnectReason::Timeout});
    CHECK(dc.reason == DisconnectReason::Timeout);
}

static TrackingState sampleTracking() {
    TrackingState t;
    t.sampleTimeUs = 1000;
    t.predictedDisplayTimeUs = 12000;
    t.headFlags = kPoseValid | kVelocityValid;
    t.head.position = {0.1f, 1.7f, -0.2f};
    const float s = std::sqrt(0.5f);
    t.head.orientation = {0, s, 0, s};
    t.controllers[0].hand = Hand::Left;
    t.controllers[1].hand = Hand::Right;
    t.controllers[1].flags = kConnected | kPoseValid;
    t.controllers[1].trigger = 0.75f;
    t.controllers[1].thumbstickX = -0.5f;
    t.controllers[1].buttons = kButtonA | kButtonTrigger;
    return t;
}

TEST(tracking_roundtrip) {
    auto t = roundtrip(sampleTracking());
    CHECK(t.predictedDisplayTimeUs == 12000);
    CHECK(t.head.position[1] == 1.7f);
    CHECK(t.controllers[1].trigger == 0.75f && t.controllers[1].buttons == (kButtonA | kButtonTrigger));
}

static bool decodes(const TrackingState& t) {
    std::vector<uint8_t> buf;
    encodeMessage(t, buf);
    return decodeMessage(buf).has_value();
}

TEST(tracking_rejects_invalid_values) {
    auto t = sampleTracking();
    t.head.orientation = {0, 0, 0, 0.5f};  // not a unit quaternion
    CHECK(!decodes(t));

    t = sampleTracking();
    t.head.position[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!decodes(t));

    t = sampleTracking();
    t.controllers[1].trigger = 1.5f;
    CHECK(!decodes(t));

    t = sampleTracking();
    t.controllers[0].hand = Hand::Right;  // both controllers right-handed
    CHECK(!decodes(t));

    t = sampleTracking();
    t.headFlags = 0x80;
    CHECK(!decodes(t));
}

TEST(messages_reject_trailing_and_truncated) {
    std::vector<uint8_t> buf;
    encodeMessage(Ping{1, 2}, buf);
    auto extra = buf;
    extra.push_back(0);
    CHECK(!decodeMessage(extra));
    CHECK(!decodeMessage(std::span(buf).first(buf.size() - 1)));
    std::vector<uint8_t> unknown{250, 0, 0};
    CHECK(!decodeMessage(unknown));
    CHECK(!decodeMessage({}));

    encodeMessage(Pong{1, 10, 50, 40}, buf);  // hostSend < hostRecv: impossible
    CHECK(!decodeMessage(buf));
}

TEST(fuzz_random_inputs_do_not_crash) {
    std::mt19937 rng(1234);
    std::vector<uint8_t> buf;
    int accepted = 0;
    for (int i = 0; i < 200000; ++i) {
        buf.resize(rng() % 300);
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        if (!buf.empty()) buf[0] = static_cast<uint8_t>(1 + rng() % 25);  // bias toward valid types
        if (decodeMessage(buf)) ++accepted;
        PacketHeader h;
        std::span<const uint8_t> p;
        (void)decodePacket(buf, h, p);
    }
    std::printf("  fuzz: %d/200000 random payloads were structurally valid\n", accepted);
    CHECK(true);
}

KT_MAIN()
