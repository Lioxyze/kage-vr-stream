#include <filesystem>

#include "kage/crypto/secure.hpp"
#include "kage/protocol/messages.hpp"
#include "test_main.hpp"

using namespace kage::crypto;
namespace kp = kage::protocol;

namespace {
struct Pair {
    SecureSession client, host;
    SessionKeys ck, hk;
    KeyPair cs, ce, hs, he;
};
Pair handshake() {
    Pair p;
    p.cs = *KeyPair::generate();
    p.ce = *KeyPair::generate();
    p.hs = *KeyPair::generate();
    p.he = *KeyPair::generate();
    p.ck = *deriveClient(p.cs, p.ce, p.hs.pub, p.he.pub);
    p.hk = *deriveHost(p.hs, p.he, p.cs.pub, p.ce.pub);
    p.client.init(p.ck);
    p.host.init(p.hk);
    return p;
}
bool deliver(SecureSession& to, const std::vector<uint8_t>& dgram, std::vector<uint8_t>& plain) {
    kp::PacketHeader h;
    std::span<const uint8_t> body;
    if (kp::decodePacket(dgram, h, body) != kp::DecodeError::None) return false;
    return to.open(dgram, h, body, plain);
}
}  // namespace

TEST(kk_handshake_agrees_and_confirms) {
    auto p = handshake();
    CHECK(p.ck.send == p.hk.recv);
    CHECK(p.ck.recv == p.hk.send);
    CHECK(p.ck.send != p.ck.recv);
    CHECK(equal16(p.ck.confirm, p.hk.confirm));
}

TEST(kk_wrong_static_key_disagrees) {
    auto p = handshake();
    auto mallory = *KeyPair::generate();  // impersonates the host without its secret
    auto bad = *deriveHost(mallory, p.he, p.cs.pub, p.ce.pub);
    CHECK(!equal16(bad.confirm, p.ck.confirm));
    CHECK(bad.recv != p.ck.send);
}

TEST(kk_rejects_low_order_key) {
    auto p = handshake();
    Key zero{};
    CHECK(!deriveClient(p.cs, p.ce, zero, p.he.pub).has_value());
}

TEST(aead_roundtrip_tamper_replay) {
    auto p = handshake();
    kp::PacketHeader h;
    h.channel = kp::Channel::Video;
    std::vector<uint8_t> msg = {1, 2, 3, 4, 5}, dgram, plain;
    p.host.seal(h, msg, dgram);
    CHECK(dgram.size() == kp::PacketHeader::kSize + msg.size() + SecureSession::kOverhead);
    CHECK(deliver(p.client, dgram, plain));
    CHECK(plain == msg);
    CHECK(!deliver(p.client, dgram, plain));  // replay

    p.host.seal(h, msg, dgram);
    auto bad = dgram;
    bad.back() ^= 1;  // MAC
    CHECK(!deliver(p.client, bad, plain));
    bad = dgram;
    bad[5] ^= 1;  // header channel byte is authenticated (Video -> other)
    CHECK(!deliver(p.client, bad, plain));
    CHECK(deliver(p.client, dgram, plain));  // the genuine one still passes

    // Wrong direction key: a client packet echoed back to the client fails.
    p.client.seal(h, msg, dgram);
    CHECK(!deliver(p.client, dgram, plain));
    CHECK(deliver(p.host, dgram, plain));
}

TEST(aead_reordering_window) {
    auto p = handshake();
    kp::PacketHeader h;
    std::vector<uint8_t> msg = {9}, plain;
    std::vector<std::vector<uint8_t>> pk(5);
    for (auto& d : pk) p.host.seal(h, msg, d);
    CHECK(deliver(p.client, pk[0], plain));
    CHECK(deliver(p.client, pk[3], plain));
    CHECK(deliver(p.client, pk[1], plain));  // late but inside the window
    CHECK(!deliver(p.client, pk[1], plain));
    CHECK(deliver(p.client, pk[2], plain));
    CHECK(deliver(p.client, pk[4], plain));
}

TEST(pairing_code_and_trust_store) {
    auto a = *KeyPair::generate(), b = *KeyPair::generate();
    CHECK(pairingCode(a.pub, b.pub) < 1'000'000);
    CHECK(pairingCode(a.pub, b.pub) == pairingCode(a.pub, b.pub));
    CHECK(fromHex(toHex(a.pub)) == a.pub);
    CHECK(!fromHex("zz").has_value());

    auto dir = std::filesystem::temp_directory_path() / "kage_test_trust";
    std::filesystem::remove_all(dir);
    auto f = dir / "paired.txt";
    CHECK(!isTrusted(f, a.pub));
    CHECK(addTrusted(f, a.pub, "Quest\nevil"));
    CHECK(isTrusted(f, a.pub));
    CHECK(!isTrusted(f, b.pub));
    CHECK(removeTrusted(f, a.pub));
    CHECK(!isTrusted(f, a.pub));

    auto k1 = loadOrCreateKeyPair(dir / "id.key");
    auto k2 = loadOrCreateKeyPair(dir / "id.key");
    CHECK(k1 && k2 && k1->pub == k2->pub);
    std::filesystem::remove_all(dir);
}

TEST(pair_messages_roundtrip) {
    kp::PairRequest r{"Quest 3S", {}};
    r.clientKey[0] = 7;
    std::vector<uint8_t> buf;
    kp::encodeMessage(r, buf);
    auto m = kp::decodeMessage(buf);
    REQUIRE(m && std::holds_alternative<kp::PairRequest>(*m));
    CHECK(std::get<kp::PairRequest>(*m).clientKey[0] == 7);
    kp::SecureAccept s;
    s.confirm[15] = 3;
    kp::encodeMessage(s, buf);
    m = kp::decodeMessage(buf);
    REQUIRE(m && std::holds_alternative<kp::SecureAccept>(*m));
    CHECK(std::get<kp::SecureAccept>(*m).confirm[15] == 3);
    buf.pop_back();
    CHECK(!kp::decodeMessage(buf).has_value());
}

KT_MAIN()
