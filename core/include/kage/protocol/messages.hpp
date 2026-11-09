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
    ConnectRequest = 30,  // client -> kage-host: please start SteamVR
    ConnectReply = 31,
    StreamHello = 40,     // client -> driver (47802), repeated until StreamConfig
    StreamConfig = 41,    // driver -> client
    IdrRequest = 42,      // client -> driver: lost reference, need a keyframe
    Haptics = 43,         // driver -> client
    ClientStats = 44,     // client -> driver, ~1 Hz, feeds bitrate control
    CalibrateFullBody = 45,  // client -> driver: start the Rift CV1 calibration (15 s)
    StreamFeatures = 46,  // both ways: optional features (client: supported, driver: active)
    BandwidthProbe = 47,  // driver -> client: burst measuring the Wi-Fi capacity at session start
    ProbeResult = 48,     // client -> driver: what arrived of that burst
    PairRequest = 32,     // client -> kage-host, repeated while the user compares the code
    PairReply = 33,
    SecureHello = 50,     // client -> driver: static + ephemeral X25519 keys (clear)
    SecureAccept = 51,    // driver -> client: ephemeral key + key confirmation (clear)
};

inline constexpr uint16_t kDefaultStreamPort = 47802;

enum CodecMask : uint8_t { kCodecH264 = 1u << 0, kCodecHEVC = 1u << 1 };

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
    kButtonThumbrest = 1u << 8,  // touch only
};

struct TrackingState {
    uint64_t sampleTimeUs = 0;            // client clock, when poses were sampled
    uint64_t predictedDisplayTimeUs = 0;  // client clock, the time poses are predicted for
    uint8_t headFlags = 0;
    Pose head;
    std::array<ControllerState, 2> controllers{};
};

using PublicKey = std::array<uint8_t, 32>;

struct ConnectRequest {
    std::string clientName;
    PublicKey clientKey{};  // must be paired with this PC
};

enum class ConnectStatus : uint8_t { Launching = 0, AlreadyRunning = 1, SteamVrMissing = 2, Failed = 3, NotPaired = 4 };

struct PairRequest {
    std::string clientName;
    PublicKey clientKey{};
};

enum class PairStatus : uint8_t { Pending = 0, Accepted = 1 };

struct PairReply {
    PairStatus status = PairStatus::Pending;
    PublicKey hostKey{};  // both sides show a code derived from the two keys
};

struct SecureHello {
    PublicKey clientStatic{}, clientEphemeral{};
};

struct SecureAccept {
    PublicKey hostEphemeral{};
    std::array<uint8_t, 16> confirm{};  // proves the driver holds the PC's static key
};

struct ConnectReply {
    ConnectStatus status = ConnectStatus::Failed;
    uint16_t streamPort = kDefaultStreamPort;
};

// Field of view as tangent-free OpenXR angles (radians): left/down negative.
struct Fov {
    float left = 0, right = 0, up = 0, down = 0;
};

struct StreamHello {
    uint32_t eyeWidth = 0;   // recommended render size per eye
    uint32_t eyeHeight = 0;
    float refreshHz = 72;
    std::array<Fov, 2> fov{};
    float ipdMeters = 0.063f;
    uint8_t codecs = kCodecHEVC | kCodecH264;  // decoders available on the client
    uint32_t maxBitrateKbps = 150000;
};

struct StreamConfig {
    uint8_t codec = kCodecHEVC;  // exactly one bit
    uint32_t width = 0;          // encoded frame (both eyes side by side)
    uint32_t height = 0;
    float refreshHz = 72;
    uint32_t bitrateKbps = 0;
};

// Optional streaming features, negotiated without breaking older builds (an unknown
// message type is ignored). Client -> driver: what it supports; driver -> client: what is on.
enum FeatureMask : uint8_t {
    kFeatureSliceUnits = 1u << 0,    // video slices sent as soon as encoded (VideoFragment kFrameSliced)
    kFeatureIntraRefresh = 1u << 1,  // encoder heals losses progressively: no keyframe wait
};
struct StreamFeatures {
    uint8_t flags = 0;
    // Foveated encoding level 0..3 (client: wanted, driver: applied to this stream). The
    // encoded image keeps full density at the centre and compresses the edges, see foveationA().
    uint8_t foveation = 0;
};
inline constexpr uint8_t kMaxFoveation = 3;
// Warp x = a*c + (1-a)*c^3 per axis (c in [-1,1], encoded -> eye). Encoded size = a * render
// size, so the centre stays 1:1 and the edges get 1 + 3(1-a)/a eye pixels per encoded pixel.
inline float foveationA(uint8_t level) {
    constexpr float k[] = {1.0f, 0.85f, 0.75f, 0.65f};
    return k[level > kMaxFoveation ? kMaxFoveation : level];
}

// Session-start capacity probe (Wi-Fi only): the driver sends `count` padded datagrams paced
// above any useful video rate, the headset reports what arrived and over which span; the
// driver starts the video at a share of that throughput instead of a fixed bitrate.
struct BandwidthProbe {
    uint32_t probeId = 0;
    uint16_t index = 0;
    uint16_t count = 0;
    uint16_t padding = 0;  // zero bytes appended (not read back: the datagram size counts)
};
struct ProbeResult {
    uint32_t probeId = 0;
    uint16_t received = 0;
    uint32_t bytes = 0;   // payload bytes received after the first datagram
    uint32_t spanUs = 0;  // first -> last arrival
};

struct IdrRequest {
    uint32_t lastFrameIndex = 0;
};

struct Haptics {
    Hand hand = Hand::Left;
    uint32_t durationUs = 0;
    float frequency = 0;  // Hz, 0 = runtime default
    float amplitude = 0;  // [0,1]
};

struct CalibrateFullBody {};

struct ClientStats {
    uint32_t framesReceived = 0;  // since previous report
    uint32_t framesDropped = 0;   // incomplete / late, since previous report
    uint32_t decodeUsAvg = 0;
    uint32_t networkUsAvg = 0;    // first fragment -> frame complete
    uint32_t rttUs = 0;
};

// Prefix of every encoded video frame (inside the reassembled frame bytes).
struct VideoFrameMeta {
    static constexpr size_t kSize = 8 + 7 * 4;
    uint64_t trackingTimeUs = 0;  // client-clock sampleTime of the TrackingState used to render
    Pose renderPose;              // head pose used by SteamVR (position + orientation only)
};

void encodeFrameMeta(const VideoFrameMeta& m, std::vector<uint8_t>& out);  // appends
bool decodeFrameMeta(std::span<const uint8_t> in, VideoFrameMeta& m);

using Message = std::variant<DiscoveryProbe, DiscoveryAnnounce, Ping, Pong, Heartbeat, Disconnect, TrackingState,
                             ConnectRequest, ConnectReply, StreamHello, StreamConfig, IdrRequest, Haptics, ClientStats,
                             PairRequest, PairReply, SecureHello, SecureAccept, CalibrateFullBody, StreamFeatures,
                             BandwidthProbe, ProbeResult>;

MessageType typeOf(const Message& m);

// Serializes `m` into `out` (cleared first).
void encodeMessage(const Message& m, std::vector<uint8_t>& out);

// Returns nullopt for unknown types, truncated/oversized bodies or invalid values.
std::optional<Message> decodeMessage(std::span<const uint8_t> payload);

}  // namespace kage::protocol
