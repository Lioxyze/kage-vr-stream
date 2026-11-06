// KAGE VR Stream - typed messages carried in packet payloads.
//
// Payload = u8 MessageType followed by the message body. Every decoder
// validates ranges (string lengths, finite floats, unit quaternions, enum
// values) and rejects trailing bytes, so malformed input never reaches the
// streaming engine.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace kage::protocol {

inline constexpr uint16_t kDefaultDiscoveryPort = 47800;
inline constexpr uint16_t kDefaultControlPort = 47801;
inline constexpr size_t kMaxNameLength = 64;

enum class MessageType : uint8_t {
    DiscoveryProbe = 1,
    DiscoveryAnnounce = 2,
    Ping = 10,
    Pong = 11,
    Heartbeat = 12,
    Disconnect = 13,
    TrackingState = 20,
};

// Client -> broadcast. "Is there a KAGE host on this LAN?"
struct DiscoveryProbe {
    uint64_t clientNonce = 0;
    std::string clientName;  // e.g. "Quest 3"
};

// Host -> client (unicast reply to a probe).
struct DiscoveryAnnounce {
    uint64_t clientNonce = 0;  // echoes the probe, lets the client match replies
    std::string hostName;
    uint16_t controlPort = kDefaultControlPort;
    // First 8 bytes of the host's long-term public key hash (Phase 2 pairing).
    // All zero until pairing is implemented.
    std::array<uint8_t, 8> hostKeyFingerprint{};
    uint32_t capabilities = 0;  // bitmask, reserved
};

// Four-timestamp exchange (NTP style) for RTT and clock offset.
struct Ping {
    uint32_t pingId = 0;
    uint64_t clientSendUs = 0;  // t0, client clock
};

struct Pong {
    uint32_t pingId = 0;
    uint64_t clientSendUs = 0;  // t0 echoed
    uint64_t hostRecvUs = 0;    // t1, host clock
    uint64_t hostSendUs = 0;    // t2, host clock
};

struct Heartbeat {
    uint64_t uptimeUs = 0;
};

enum class DisconnectReason : uint8_t {
    UserRequested = 0,
    Shutdown = 1,
    Timeout = 2,
    ProtocolError = 3,
};

struct Disconnect {
    DisconnectReason reason = DisconnectReason::UserRequested;
};

// OpenXR conventions: right-handed, Y up, -Z forward, meters, quaternion xyzw.
struct Pose {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 4> orientation{0, 0, 0, 1};
    std::array<float, 3> linearVelocity{0, 0, 0};
    std::array<float, 3> angularVelocity{0, 0, 0};
};

enum TrackingFlags : uint8_t {
    kPoseValid = 1u << 0,
    kVelocityValid = 1u << 1,
    kConnected = 1u << 2,  // controllers only
};

enum class Hand : uint8_t { Left = 0, Right = 1 };

struct ControllerState {
    Hand hand = Hand::Left;
    uint8_t flags = 0;
    Pose pose;
    uint32_t buttons = 0;  // bitmask, see ControllerButton
    uint32_t touches = 0;  // capacitive touch bitmask
    float trigger = 0;     // [0,1]
    float grip = 0;        // [0,1]
    float thumbstickX = 0; // [-1,1]
    float thumbstickY = 0; // [-1,1]
};

enum ControllerButton : uint32_t {
    kButtonA = 1u << 0,  // right
    kButtonB = 1u << 1,  // right
    kButtonX = 1u << 2,  // left
    kButtonY = 1u << 3,  // left
    kButtonMenu = 1u << 4,
    kButtonThumbstick = 1u << 5,
    kButtonTrigger = 1u << 6,
    kButtonGrip = 1u << 7,
};

struct TrackingState {
    uint64_t sampleTimeUs = 0;            // client clock, when poses were sampled
    uint64_t predictedDisplayTimeUs = 0;  // client clock, the time poses are predicted for
    uint8_t headFlags = 0;
    Pose head;
    std::array<ControllerState, 2> controllers{};
};

using Message = std::variant<DiscoveryProbe, DiscoveryAnnounce, Ping, Pong, Heartbeat, Disconnect, TrackingState>;

MessageType typeOf(const Message& m);

// Serializes `m` into `out` (cleared first).
void encodeMessage(const Message& m, std::vector<uint8_t>& out);

// Returns nullopt for unknown types, truncated/oversized bodies or invalid values.
std::optional<Message> decodeMessage(std::span<const uint8_t> payload);

}  // namespace kage::protocol
