#pragma once

// Use the original 823471F8 canvas rebuild and 8216FEA8 vertex transform as
// the geometry oracle. These private guest fixtures never run the game loop.
#include "runtime/native/display_mode.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" PPC_FUNC(__imp__sub_823471F8);
extern "C" PPC_FUNC(__imp__sub_8216FEA8);

namespace TitleTextLayoutTestDetail {
using namespace DarkRecomp::Native;
constexpr uint32_t contextBytes = 704;
using ContextBytes = std::array<uint8_t, contextBytes>;
using Matrix = std::array<uint32_t, 16>;
using Vertices = std::array<std::array<float, 3>, 7>;

struct Fixture {
    uint32_t block = memory->allocate(4096);
    NativeVideoMode oldMode = nativeVideoMode();
    PPCContext* oldContext = currentContext;
    Fixture() { check(block != 0, "Cannot allocate title text layout fixture"); }
    ~Fixture() {
        currentContext = oldContext;
        setNativeVideoMode(oldMode.width, oldMode.height);
        memory->release(block);
    }
};

inline void putFloat(uint32_t address, float value) {
    memory->write32(address, std::bit_cast<uint32_t>(value));
}
inline float getFloat(uint32_t address) {
    return std::bit_cast<float>(memory->read32(address));
}
inline ContextBytes snapshot(uint32_t block) {
    ContextBytes bytes{};
    std::memcpy(bytes.data(), memory->base() + block, bytes.size());
    return bytes;
}
inline Matrix matrix(uint32_t block) {
    Matrix result{};
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = memory->read32(block + 272 + i * 4);
    return result;
}
inline void restoreMatrix(uint32_t block, const Matrix& original) {
    for (unsigned i = 0; i < original.size(); ++i)
        memory->write32(block + 272 + i * 4, original[i]);
}
inline void layoutNear(double actual, double expected, const char* message) {
    check(std::isfinite(actual) && std::abs(actual - expected) < .002, message);
}

inline void rebuild(const PPCContext& initial, uint32_t block, NativeVideoMode mode) {
    std::memset(memory->base() + block, 0, contextBytes);
    const float logicalScale = float(mode.height) / 480;
    putFloat(block + 336, logicalScale);
    putFloat(block + 340, logicalScale);
    // Keep the original CView cache clean and supply its full viewport size.
    putFloat(block + 632, float(mode.width));
    putFloat(block + 636, float(mode.height));
    putFloat(block + 656, getFloat(0x82A480C4) + 1);
    memory->write32(block + 676, mode.width);
    memory->write32(block + 680, mode.height);
    PPCContext guest;
    std::memcpy(&guest, &initial, sizeof guest);
    guest.r3.u64 = block;
    guest.lr = 0x8233F13C;
    auto* previousContext = currentContext;
    currentContext = &guest;
    __imp__sub_823471F8(guest, memory->base());
    currentContext = previousContext;
    check(guest.r1.u32 == initial.r1.u32 && uint32_t(guest.lr) == 0x8233F13C,
          "Original title matrix rebuild changed stack or return address");
    check(getFloat(block + 336) == logicalScale && getFloat(block + 340) == logicalScale,
          "Original title matrix rebuild changed logical scales");
    // Metadata outside the matrix must survive fitting byte for byte.
    memory->write32(block + 344, 0x3F19999A);
    memory->write32(block + 348, 0x3F4CCCCD);
    memory->write32(block + 688, 0xA17E59C3);
    memory->write32(block + 692, 0x01234567);
    memory->write32(block + 696, 0x89ABCDEF);
    memory->write32(block + 700, 0x76543210);
}

inline Vertices transform(const PPCContext& initial, uint32_t block) {
    // The first four points describe the complete authored canvas. The last
    // three describe perpendicular edges of one square glyph inside it.
    constexpr Vertices authored{{{0,0,0}, {853,0,0}, {853,480,0}, {0,480,0},
                                 {400,200,0}, {410,200,0}, {400,210,0}}};
    const uint32_t address = block + 1024;
    for (unsigned i = 0; i < authored.size(); ++i)
        for (unsigned c = 0; c < 3; ++c) putFloat(address + i * 12 + c * 4, authored[i][c]);
    PPCContext guest;
    std::memcpy(&guest, &initial, sizeof guest);
    guest.r3.u64 = guest.r4.u64 = address;
    guest.r5.u64 = block + 272;
    guest.r6.u64 = authored.size();
    auto* previousContext = currentContext;
    currentContext = &guest;
    __imp__sub_8216FEA8(guest, memory->base());
    currentContext = previousContext;
    check(guest.r1.u32 == initial.r1.u32, "Original title vertex transform changed stack");
    Vertices output{};
    for (unsigned i = 0; i < output.size(); ++i)
        for (unsigned c = 0; c < 3; ++c) output[i][c] = getFloat(address + i * 12 + c * 4);
    return output;
}

inline bool fit(uint32_t block) {
    const auto before = snapshot(block);
    const bool fitted = fitTitleTextMatrix(block);
    const auto after = snapshot(block);
    for (unsigned offset = 0; offset < contextBytes; ++offset) {
        const bool xyColumn = offset >= 272 && offset < 336 && (offset - 272) % 16 < 8;
        if (!fitted || !xyColumn)
            check(before[offset] == after[offset], "Title text fitting changed logical scales, depth, W or metadata");
    }
    return fitted;
}
} // namespace TitleTextLayoutTestDetail

static void testTitleTextLayout(PPCContext& ctx) {
    using namespace TitleTextLayoutTestDetail;
    Fixture fixture;
    unsigned geometries = 0;
    for (const NativeVideoMode mode : std::array<NativeVideoMode, 8>{{
            {1152,720}, {960,720}, {2304,1440}, {1920,1440}, {320,720},
            {1280,720}, {1720,720}, {2560,720}}}) {
        check(setNativeVideoMode(mode.width, mode.height), "Title text video mode rejected");
        rebuild(ctx, fixture.block, mode);
        const auto original = matrix(fixture.block);
        const auto originalVertices = transform(ctx, fixture.block);
        const bool narrow = uint64_t(mode.width) * 9 < uint64_t(mode.height) * 16;
        if (narrow)
            check((double(originalVertices[1][0]) + 1) * mode.width * .5 > mode.width,
                  "Original title fixture does not reproduce the clipped right edge");
        check(fit(fixture.block) == narrow, "Title text fitting did not match the narrow viewport scope");
        if (!narrow) {
            check(matrix(fixture.block) == original, "16:9 or ultrawide title text changed");
            continue;
        }
        const auto fitted = matrix(fixture.block);
        const auto vertices = transform(ctx, fixture.block);
        // Independent physical oracle: fit the 853x480 authored canvas by
        // width and divide the remaining height equally above and below it.
        const double pixelScale = double(mode.width) / 853;
        const double margin = (mode.height - 480 * pixelScale) * .5;
        const auto px = [&](unsigned i) { return (double(vertices[i][0]) + 1) * mode.width * .5; };
        const auto py = [&](unsigned i) { return (double(vertices[i][1]) + 1) * mode.height * .5; };
        layoutNear(px(0), 0, "Title canvas left edge does not fit the viewport");
        layoutNear(px(1), mode.width, "Title canvas right edge is clipped");
        layoutNear(px(2), mode.width, "Title canvas lower right edge is clipped");
        layoutNear(px(3), 0, "Title canvas lower left edge moved");
        layoutNear(py(0), margin, "Title canvas upper margin is wrong");
        layoutNear(py(2), mode.height - margin, "Title canvas lower margin is wrong");
        layoutNear(py(0) + py(2), mode.height, "Title canvas vertical fitting is asymmetric");
        layoutNear(px(5) - px(4), 10 * pixelScale, "Title glyph horizontal scale is wrong");
        layoutNear(py(6) - py(4), 10 * pixelScale, "Title glyph was stretched vertically");
        // Resolution-scale 2 applies after this logical geometry, exactly once.
        layoutNear(px(1) * 2, mode.width * 2, "Scale-2 title canvas lost its right edge");
        layoutNear(py(0) * 2, margin * 2, "Scale-2 title margin changed");
        for (unsigned i = 0; i < vertices.size(); ++i)
            check(std::bit_cast<uint32_t>(vertices[i][2]) == std::bit_cast<uint32_t>(originalVertices[i][2]),
                  "Title text fitting changed original guest depth");
        // The painter restores its borrowed transform. A later draw or retail
        // rebuild must yield the same fit rather than applying scale twice.
        restoreMatrix(fixture.block, original);
        check(fit(fixture.block) && matrix(fixture.block) == fitted, "Restored title matrix compounded fitting");
        rebuild(ctx, fixture.block, mode);
        check(fit(fixture.block) && matrix(fixture.block) == fitted, "Original title rebuild compounded fitting");
        ++geometries;
    }

    const NativeVideoMode mode{1152,720};
    check(setNativeVideoMode(mode.width, mode.height), "Cannot select title rejection fixture mode");
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        rebuild(ctx, fixture.block, mode);
        if (invalid == 0) memory->write32(fixture.block + 676, 1280); // Another viewport.
        if (invalid == 1) memory->write32(fixture.block + 668, 16);   // A subview.
        if (invalid == 2) putFloat(fixture.block + 336, float(mode.width) / 640); // Menu canvas.
        if (invalid == 3) putFloat(fixture.block + 340, 2);          // Different height canvas.
        if (invalid == 4) putFloat(fixture.block + 336, std::numeric_limits<float>::quiet_NaN());
        if (invalid == 5) putFloat(fixture.block + 272, std::numeric_limits<float>::infinity());
        if (invalid == 6) memory->write32(fixture.block + 680, 480);
        check(!fit(fixture.block), "Title text fitting accepted an unrelated or invalid context");
    }
    check(!fitTitleTextMatrix(0) && !fitTitleTextMatrix(0xFFFFFFF0), "Title text fitting accepted an invalid guest span");
    std::printf("TitleTextLayoutContract: original guest canvas/vertex transforms; %u narrow canvases, symmetric margins, square glyphs, scale-2, depth/metadata, rebuilds, 16:9/ultrawide and invalid scope passed.\n", geometries);
}
