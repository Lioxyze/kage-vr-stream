// KAGE VR Stream - splitting encoded video frames into datagrams and
// reassembling them on the receiver.
//
// Policy is latency-first: a frame is delivered only when complete; once a
// newer frame completes, every older incomplete frame is discarded (never
// waited for) and the receiver flags that a keyframe / reference refresh is
// required. Memory is bounded (kMaxFrameSize, kMaxInFlight).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "kage/protocol/packet.hpp"

namespace kage::protocol {

struct VideoFragmentHeader {
    static constexpr size_t kSize = 25;

    uint32_t frameIndex = 0;
    uint32_t frameSize = 0;       // total encoded bytes of the frame
    uint32_t fragmentOffset = 0;  // byte offset of this fragment in the frame
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint64_t poseTimeUs = 0;  // tracking sample the frame was rendered with
    uint8_t flags = 0;        // VideoFrameFlags
};

enum VideoFrameFlags : uint8_t {
    kFrameKeyframe = 1u << 0,
    // FEC parity datagram: XOR of an interleaved group of data fragments (fragment i belongs
    // to group i % groups). fragmentIndex = group, fragmentOffset = number of groups.
    // One lost fragment per group is rebuilt without waiting for a keyframe.
    kFrameParity = 1u << 1,
};

inline constexpr uint32_t kMaxFrameSize = 8u * 1024u * 1024u;  // 8 MiB per encoded frame
inline constexpr size_t kMaxVideoPayload =
    kMaxDatagram - PacketHeader::kSize - VideoFragmentHeader::kSize - 24 /* crypto::SecureSession::kOverhead */;

// Produces the payloads (fragment header + data) for one encoded frame. Each
// payload fits in kMaxVideoPayload + VideoFragmentHeader::kSize bytes.
void fragmentFrame(uint32_t frameIndex, uint64_t poseTimeUs, uint8_t flags, std::span<const uint8_t> frame,
                   const std::function<void(std::span<const uint8_t>)>& emit, size_t maxPayload = kMaxVideoPayload,
                   unsigned fecGroup = 0);  // 0 = no FEC; N = one parity datagram per N data fragments

struct CompletedFrame {
    uint32_t frameIndex = 0;
    uint64_t poseTimeUs = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> data;
};

struct ReassemblyStats {
    uint64_t fragmentsReceived = 0;
    uint64_t fragmentsRejected = 0;  // malformed / inconsistent
    uint64_t fragmentsDuplicate = 0;
    uint64_t fragmentsStale = 0;     // belong to an already superseded frame
    uint64_t framesCompleted = 0;
    uint64_t framesDropped = 0;      // incomplete frames discarded
    uint64_t fragmentsRecovered = 0;  // rebuilt from FEC parity
};

class FrameReassembler {
public:
    static constexpr size_t kMaxInFlight = 4;

    // Feeds one video payload. Returns a frame when it becomes complete.
    std::optional<CompletedFrame> push(std::span<const uint8_t> payload);

    // True after any frame was dropped; cleared by acknowledgeKeyframeRequest().
    bool keyframeNeeded() const { return keyframeNeeded_; }
    void acknowledgeKeyframeRequest() { keyframeNeeded_ = false; }

    const ReassemblyStats& stats() const { return stats_; }

private:
    struct Slot {
        bool used = false;
        uint32_t frameIndex = 0;
        uint32_t frameSize = 0;
        uint16_t fragmentCount = 0;
        uint16_t received = 0;
        uint64_t poseTimeUs = 0;
        uint8_t flags = 0;
        std::vector<uint8_t> data;
        std::vector<bool> have;
        uint16_t groups = 0;   // FEC groups (0 until a parity datagram arrives)
        size_t parityLen = 0;  // = data fragment size
        size_t fragLen = 0;    // size of the full-size data fragments seen so far
        std::vector<std::vector<uint8_t>> parity;
        std::vector<uint16_t> groupHave;  // data fragments received per group
    };

    Slot* findOrCreate(const VideoFragmentHeader& h);
    void dropOlderThan(uint32_t frameIndex);
    void tryRecover(Slot& s, uint16_t group);

    Slot slots_[kMaxInFlight];
    bool haveDelivered_ = false;
    uint32_t lastDelivered_ = 0;
    bool keyframeNeeded_ = false;
    ReassemblyStats stats_;
};

}  // namespace kage::protocol
