#pragma once

#include <cstdint>

namespace DarkRecomp::Native {

// One decoded index chunk from 8225F320, consumed at 8225DE78's indexed draw.
// The original decoder and draw still execute; this only retains the transient
// guest index address until the draw observer copies the index data.
class DecodedDrawRequest {
public:
    void record(uint32_t indices, uint32_t capacity, uint32_t produced, uint32_t byteOffset) noexcept {
        clear();
        if (!indices || !capacity || capacity > 8192 || !produced || produced > capacity ||
            produced % 3 || (byteOffset & 1) || uint64_t(indices) + uint64_t(produced) * 2 > 0x100000000ull)
            return;
        indices_ = indices;
        count_ = produced;
        firstIndex_ = byteOffset / 2;
    }

    // Every matching draw-site call is a consumption attempt. A shape
    // mismatch invalidates the chunk so a later call cannot reuse stale data.
    uint32_t consume(uint32_t returnAddress, uint32_t primitive, uint32_t baseVertex,
                     uint32_t firstIndex, uint32_t count) noexcept {
        const bool match = indices_ && returnAddress == 0x8225E200 && primitive == 4 && !baseVertex &&
            firstIndex == firstIndex_ && count == count_;
        const uint32_t indices = match ? indices_ : 0;
        clear();
        return indices;
    }

private:
    void clear() noexcept { indices_ = count_ = firstIndex_ = 0; }

    uint32_t indices_ = 0;
    uint32_t count_ = 0;
    uint32_t firstIndex_ = 0;
};

} // namespace DarkRecomp::Native
