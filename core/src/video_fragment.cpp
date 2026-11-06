#include "kage/protocol/video_fragment.hpp"

#include <algorithm>
#include <cstring>

#include "kage/protocol/wire.hpp"

namespace kage::protocol {

void fragmentFrame(uint32_t frameIndex, uint64_t poseTimeUs, uint8_t flags, std::span<const uint8_t> frame,
                   const std::function<void(std::span<const uint8_t>)>& emit, size_t maxPayload) {
    if (frame.empty() || frame.size() > kMaxFrameSize || maxPayload == 0) return;
    const size_t count = (frame.size() + maxPayload - 1) / maxPayload;
    if (count > 0xFFFF) return;

    std::vector<uint8_t> buf;
    buf.reserve(VideoFragmentHeader::kSize + maxPayload);
    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * maxPayload;
        const size_t len = std::min(maxPayload, frame.size() - off);
        buf.clear();
        wire::Writer w(buf);
        w.u32(frameIndex);
        w.u32(static_cast<uint32_t>(frame.size()));
        w.u32(static_cast<uint32_t>(off));
        w.u16(static_cast<uint16_t>(i));
        w.u16(static_cast<uint16_t>(count));
        w.u64(poseTimeUs);
        w.u8(flags);
        w.bytes(frame.subspan(off, len));
        emit(buf);
    }
}

FrameReassembler::Slot* FrameReassembler::findOrCreate(const VideoFragmentHeader& h) {
    for (auto& s : slots_)
        if (s.used && s.frameIndex == h.frameIndex) return &s;

    // Pick a free slot, otherwise evict the oldest in-flight frame.
    Slot* victim = nullptr;
    for (auto& s : slots_) {
        if (!s.used) {
            victim = &s;
            break;
        }
        if (!victim || seqNewer(victim->frameIndex, s.frameIndex)) victim = &s;
    }
    if (victim->used) {
        if (seqNewer(victim->frameIndex, h.frameIndex)) return nullptr;  // incoming is older than everything
        ++stats_.framesDropped;
        keyframeNeeded_ = true;
    }
    victim->used = true;
    victim->frameIndex = h.frameIndex;
    victim->frameSize = h.frameSize;
    victim->fragmentCount = h.fragmentCount;
    victim->received = 0;
    victim->poseTimeUs = h.poseTimeUs;
    victim->flags = h.flags;
    victim->data.assign(h.frameSize, 0);
    victim->have.assign(h.fragmentCount, false);
    return victim;
}

void FrameReassembler::dropOlderThan(uint32_t frameIndex) {
    for (auto& s : slots_) {
        if (s.used && seqNewer(frameIndex, s.frameIndex)) {
            s.used = false;
            ++stats_.framesDropped;
            keyframeNeeded_ = true;
        }
    }
}

std::optional<CompletedFrame> FrameReassembler::push(std::span<const uint8_t> payload) {
    ++stats_.fragmentsReceived;
    wire::Reader r(payload);
    VideoFragmentHeader h;
    r.u32(h.frameIndex);
    r.u32(h.frameSize);
    r.u32(h.fragmentOffset);
    r.u16(h.fragmentIndex);
    r.u16(h.fragmentCount);
    r.u64(h.poseTimeUs);
    r.u8(h.flags);
    const auto data = r.rest();

    const bool valid = r.ok() && h.frameSize > 0 && h.frameSize <= kMaxFrameSize && h.fragmentCount > 0 &&
                       h.fragmentIndex < h.fragmentCount && !data.empty() &&
                       static_cast<uint64_t>(h.fragmentOffset) + data.size() <= h.frameSize &&
                       (h.flags & ~kFrameKeyframe) == 0;
    if (!valid) {
        ++stats_.fragmentsRejected;
        return std::nullopt;
    }
    if (haveDelivered_ && !seqNewer(h.frameIndex, lastDelivered_)) {
        ++stats_.fragmentsStale;
        return std::nullopt;
    }

    Slot* s = findOrCreate(h);
    if (!s) {
        ++stats_.fragmentsStale;
        return std::nullopt;
    }
    if (s->frameSize != h.frameSize || s->fragmentCount != h.fragmentCount || s->flags != h.flags ||
        s->poseTimeUs != h.poseTimeUs) {
        ++stats_.fragmentsRejected;  // inconsistent with earlier fragments of this frame
        return std::nullopt;
    }
    if (s->have[h.fragmentIndex]) {
        ++stats_.fragmentsDuplicate;
        return std::nullopt;
    }
    std::memcpy(s->data.data() + h.fragmentOffset, data.data(), data.size());
    s->have[h.fragmentIndex] = true;
    ++s->received;
    if (s->received != s->fragmentCount) return std::nullopt;

    CompletedFrame done;
    done.frameIndex = s->frameIndex;
    done.poseTimeUs = s->poseTimeUs;
    done.flags = s->flags;
    done.data = std::move(s->data);
    s->used = false;

    dropOlderThan(done.frameIndex);  // older incomplete frames are now useless
    haveDelivered_ = true;
    lastDelivered_ = done.frameIndex;
    ++stats_.framesCompleted;
    if (done.flags & kFrameKeyframe) keyframeNeeded_ = false;
    return done;
}

}  // namespace kage::protocol
