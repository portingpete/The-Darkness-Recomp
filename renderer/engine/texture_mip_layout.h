#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace DarkRecomp {

// Native transcription of the borderless 2D/cube branches of
// original 82863F68 (ppc_recomp.102.cpp). Input words are host-endian values
// loaded from resource +28..+51, NOT the first six resource words.
// 828AE188 clears word1/word5 addresses and returns originalOffsetBytes.
//
// Mapping:
// * Ordinary mip0 uses descriptor pitch and baseAddress.
// * Mip1+ starts afresh at mipAddress, using next-power-of-two dimensions.
// * If packedTail starts at mip0, ALL levels instead use baseAddress.
// * Each preceding mip occupies faceStride * faces; each face starts on a
//   4 KiB boundary. Cube mips are grouped by level, not by face.
// * 82863848 returns a LINEAR packed offset. Tiled reads must instead use
//   allocationAddress + surfaceOffsetBytes + worldTextureTiledOffset(
//       x + originBlockX, y + originBlockY, pitchBlocks, bytesPerBlock).
//   Never add packedOffsetBytes to a tiled byte address.
//
// This describes storage, not sampler LOD selection. The caller decides which
// authored levels to request (word4's min/max LOD fields are not layout inputs).
// Unsupported descriptors and 32-bit address overflow fail without changing out.
struct TextureMipLayout {
    uint32_t width{}, height{}, blocksWide{}, blocksHigh{};
    uint32_t blockTexels{}, bytesPerBlock{}, pitchBlocks{}, rowPitchBytes{};
    uint32_t faces{}, faceStrideBytes{};
    uint32_t allocationAddress{}, surfaceOffsetBytes{};
    uint32_t originBlockX{}, originBlockY{}, packedOffsetBytes{};
    uint32_t originalOffsetBytes{};
    bool usesMipAddress{}, packed{};
};

inline const char* getTextureMipLayout(const uint32_t (&fetch)[6],
                                     uint32_t face, uint32_t mip,
                                     TextureMipLayout& out) {
    const uint32_t dimension = (fetch[5] >> 9) & 3;
    const bool tiled = (fetch[0] & 0x80000000u) != 0;
    if ((fetch[0] & 3) != 2 ||
        (dimension != 1 && dimension != 3))
        return "texture mip layout requires 2D or cube fetch";
    if ((fetch[3] & 0x80000000u) || (fetch[1] & 0x400))
        return "bordered/array texture mip layout is unsupported";

    TextureMipLayout layout;
    const uint32_t format = fetch[1] & 63;
    switch (format) {
    case 2:  layout.blockTexels = 1; layout.bytesPerBlock = 1; break;
    case 6:  layout.blockTexels = 1; layout.bytesPerBlock = 4; break;
    case 18: layout.blockTexels = 4; layout.bytesPerBlock = 8; break;
    case 20: case 49: layout.blockTexels = 4; layout.bytesPerBlock = 16; break;
    default: return "unsupported texture mip format";
    }
    const uint32_t width = (fetch[2] & 8191) + 1;
    const uint32_t height = ((fetch[2] >> 13) & 8191) + 1;
    layout.faces = dimension == 3 ? 6 : 1;
    if (face >= layout.faces) return "texture mip face out of range";
    // 8285AA08 / 828641CC use the encoded slice count for preceding cube
    // levels. Accept standard six-face cubes only, rather than assume six.
    if (dimension == 3 && (width != height || !std::has_single_bit(width) ||
                           (fetch[2] >> 26) != 5))
        return "unsupported cube dimensions or face count";
    if (dimension == 1 && (fetch[2] >> 26))
        return "2D texture has nonzero slice count";
    if (mip >= std::bit_width((std::max)(width, height)))
        return "texture mip level out of range";

    const auto align = [](uint64_t n, uint32_t a) {
        return (n + a - 1) & ~uint64_t(a - 1);
    };
    const auto shrink = [](uint32_t n, uint32_t level) {
        return (std::max)(1u, n >> level);
    };
    const uint32_t block = layout.blockTexels;
    const uint32_t roundedWidth = std::bit_ceil(width);
    const uint32_t roundedHeight = std::bit_ceil(height);
    const uint32_t minLog = (std::min)(std::bit_width(roundedWidth),
                                      std::bit_width(roundedHeight)) - 1;
    const uint32_t tailStart = minLog > 4 ? minLog - 4 : 0;
    const bool packedTail = (fetch[5] & 0x800) != 0;
    const bool packedBase = packedTail && tailStart == 0;
    layout.usesMipAddress = mip != 0 && !packedBase;
    layout.allocationAddress = fetch[layout.usesMipAddress ? 5 : 1] & 0xfffff000u;
    if (!layout.allocationAddress) return "texture mip allocation is missing";
    layout.width = shrink(width, mip);
    layout.height = shrink(height, mip);
    layout.blocksWide = (layout.width + block - 1) / block;
    layout.blocksHigh = (layout.height + block - 1) / block;

    uint64_t preceding = 0, stride = 0;
    if (!mip && !packedBase) {
        layout.pitchBlocks = (((fetch[0] >> 22) & 511) * 32) / block;
        if (layout.pitchBlocks < layout.blocksWide || (tiled && (layout.pitchBlocks & 31)))
            return "texture mip base pitch is invalid";
        layout.rowPitchBytes = layout.pitchBlocks * layout.bytesPerBlock;
        // 82863320 pads height in texels by 32 * block height; 82864340
        // applies the descriptor pitch (not nextPow2(width)) to base faces.
        const uint32_t heightAlignment = !tiled && dimension == 1 &&
            !packedTail && !(fetch[4] & 0x3c0) ? 1 : 32;
        stride = align(uint64_t(layout.rowPitchBytes) *
                       align(layout.blocksHigh, heightAlignment), 4096);
    } else {
        // 82864224 visits mip1 first, or mip0 when the base is packed.
        // 82863320's texel padding is exactly 32 blocks on each axis.
        for (uint32_t level = packedBase ? 0 : 1; level <= mip; ++level) {
            const uint32_t w = shrink(roundedWidth, level);
            const uint32_t h = shrink(roundedHeight, level);
            const uint32_t pitchAlignment = tiled ? 32 : (std::max)(32u, 256 / layout.bytesPerBlock);
            layout.pitchBlocks = uint32_t(align(w, pitchAlignment * block)) / block;
            layout.rowPitchBytes = layout.pitchBlocks * layout.bytesPerBlock;
            stride = align(uint64_t(layout.rowPitchBytes) *
                           (align(h, 32 * block) / block), 4096);
            if (packedTail && (w <= 16 || h <= 16)) {
                layout.packed = true;
                const uint32_t relative = mip - level;
                uint32_t x = 0, y = 0;
                // 82863848 chooses X when width <= height, Y otherwise
                // for the first three levels, then switches axes.
                if (relative < 3) {
                    if (w <= h) x = 16u >> relative;
                    else y = 16u >> relative;
                } else {
                    if (w <= h) y = h >> (relative - 2);
                    else x = w >> (relative - 2);
                    // The original's further Z packing belongs to volume
                    // levels; a valid 2D/cube chain never reaches that branch.
                    if ((std::max)(x, y) < 4)
                        return "texture mip tail exceeds 2D packing range";
                }
                layout.originBlockX = x / block;
                layout.originBlockY = y / block;
                layout.packedOffsetBytes = layout.originBlockY * layout.rowPitchBytes +
                                           layout.originBlockX * layout.bytesPerBlock;
                break;
            }
            if (level == mip) break;
            preceding += stride * layout.faces;
        }
    }
    const uint64_t surface = preceding + stride * face;
    const uint64_t original = surface + layout.packedOffsetBytes;
    if (stride > UINT32_MAX || original > UINT32_MAX ||
        uint64_t(layout.allocationAddress) + surface + stride > 0x100000000ull)
        return "texture mip address overflow";
    if (layout.originBlockX + layout.blocksWide > layout.pitchBlocks ||
        uint64_t(layout.originBlockY + layout.blocksHigh) * layout.rowPitchBytes > stride)
        return "texture mip rectangle exceeds padded surface";
    layout.faceStrideBytes = uint32_t(stride);
    layout.surfaceOffsetBytes = uint32_t(surface);
    layout.originalOffsetBytes = uint32_t(original);
    out = layout;
    return nullptr;
}

} // namespace DarkRecomp
