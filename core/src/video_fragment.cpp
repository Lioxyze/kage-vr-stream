#include "kage/protocol/video_fragment.hpp"

#include <algorithm>
#include <cstring>

#include "kage/protocol/wire.hpp"

namespace kage::protocol {

void fragmentFrame(uint32_t frameIndex, uint64_t poseTimeUs, uint8_t flags, std::span<const uint8_t> frame,
                   const std::function<void(std::span<const uint8_t>)>& emit, size_t maxPayload,
                   unsigned fecGroup) {
    if (frame.empty() || frame.size() > kMaxFrameSize || maxPayload == 0) return;
    const size_t count = (frame.size() + maxPayload - 1) / maxPayload;
    if (count > 0xFFFF) return;

    std::vector<uint8_t> buf;
    buf.reserve(VideoFragmentHeader::kSize + maxPayload);
    const size_t groups = (fecGroup && count >= 2) ? (count + fecGroup - 1) / fecGroup : 0;
    std::vector<std::vector<uint8_t>> parity(groups, std::vector<uint8_t>(maxPayload, 0));
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
        if (groups) {
            auto& p = parity[i % groups];
            for (size_t k = 0; k < len; ++k) p[k] ^= frame[off + k];
        }
    }
    for (size_t g = 0; g < groups; ++g) {
        buf.clear();
        wire::Writer w(buf);
        w.u32(frameIndex);
        w.u32(static_cast<uint32_t>(frame.size()));
        w.u32(static_cast<uint32_t>(groups));
        w.u16(static_cast<uint16_t>(g));
        w.u16(static_cast<uint16_t>(count));
        w.u64(poseTimeUs);
        w.u8(static_cast<uint8_t>(flags | kFrameParity));
        w.bytes(parity[g]);
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
    victim->groups = 0;
    victim->parityLen = 0;
    victim->fragLen = 0;
    victim->parity.clear();
    victim->groupHave.clear();
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

void FrameReassembler::tryRecover(Slot& s, uint16_t g) {
    if (s.parity[g].empty() || s.fragLen != s.parityLen) return;  // sizes must agree to rebuild
    const uint16_t members = static_cast<uint16_t>((s.fragmentCount - g + s.groups - 1) / s.groups);
    if (s.groupHave[g] + 1u != members) return;  // nothing missing, or more than one: no rebuild
    uint16_t missing = 0;
    for (uint16_t i = g; i < s.fragmentCount; i = static_cast<uint16_t>(i + s.groups))
        if (!s.have[i]) missing = i;
    std::vector<uint8_t> rebuilt = s.parity[g];
    for (uint16_t i = g; i < s.fragmentCount; i = static_cast<uint16_t>(i + s.groups)) {
        if (i == missing) continue;
        const size_t off = static_cast<size_t>(i) * s.parityLen;
        const size_t len = std::min(s.parityLen, static_cast<size_t>(s.frameSize) - off);
        for (size_t k = 0; k < len; ++k) rebuilt[k] ^= s.data[off + k];
    }
    const size_t off = static_cast<size_t>(missing) * s.parityLen;
    std::memcpy(s.data.data() + off, rebuilt.data(), std::min(s.parityLen, static_cast<size_t>(s.frameSize) - off));
    s.have[missing] = true;
    ++s.received;
    ++s.groupHave[g];
    ++stats_.fragmentsRecovered;
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

    const bool isParity = (h.flags & kFrameParity) != 0;
    h.flags &= static_cast<uint8_t>(~kFrameParity);
    const bool common = r.ok() && h.frameSize > 0 && h.frameSize <= kMaxFrameSize && h.fragmentCount > 0 &&
                        !data.empty() && (h.flags & ~kFrameKeyframe) == 0;
    // Parity: fragmentOffset = groups, fragmentIndex = group; its length is the fragment size,
    // which must cover the frame with fragmentCount fragments.
    const bool valid =
        common && (isParity ? (h.fragmentOffset >= 1 && h.fragmentOffset <= h.fragmentCount &&
                               h.fragmentIndex < h.fragmentOffset &&
                               static_cast<uint64_t>(data.size()) * h.fragmentCount >= h.frameSize &&
                               static_cast<uint64_t>(data.size()) * (h.fragmentCount - 1) < h.frameSize)
                            : (h.fragmentIndex < h.fragmentCount &&
                               static_cast<uint64_t>(h.fragmentOffset) + data.size() <= h.frameSize));
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
    if (isParity) {
        if (s->groups == 0) {  // first parity of this frame: set up the groups
            s->groups = static_cast<uint16_t>(h.fragmentOffset);
            s->parityLen = data.size();
            s->parity.assign(s->groups, {});
            s->groupHave.assign(s->groups, 0);
            for (uint16_t i = 0; i < s->fragmentCount; ++i)
                if (s->have[i]) ++s->groupHave[i % s->groups];
        }
        if (h.fragmentOffset != s->groups || data.size() != s->parityLen) {
            ++stats_.fragmentsRejected;
            return std::nullopt;
        }
        if (!s->parity[h.fragmentIndex].empty()) {
            ++stats_.fragmentsDuplicate;
            return std::nullopt;
        }
        s->parity[h.fragmentIndex].assign(data.begin(), data.end());
        tryRecover(*s, h.fragmentIndex);
    } else {
        if (s->have[h.fragmentIndex]) {
            ++stats_.fragmentsDuplicate;
            return std::nullopt;
        }
        if (h.fragmentIndex + 1 < h.fragmentCount) {  // full-size fragment: fixed size, offset = index * size
            if ((s->fragLen && data.size() != s->fragLen) ||
                static_cast<uint64_t>(h.fragmentIndex) * data.size() != h.fragmentOffset) {
                ++stats_.fragmentsRejected;
                return std::nullopt;
            }
            s->fragLen = data.size();
        }
        std::memcpy(s->data.data() + h.fragmentOffset, data.data(), data.size());
        s->have[h.fragmentIndex] = true;
        ++s->received;
        if (s->groups) {
            const uint16_t g = h.fragmentIndex % s->groups;
            ++s->groupHave[g];
            tryRecover(*s, g);
        }
    }
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
