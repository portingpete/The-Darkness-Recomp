#pragma once

// Parent integration: include after native_tests.cpp's check(), and invoke
// testTextureMipLayout(ctx) after Memory::load, beside testTextureTileLayout.
// Requires the ORIGINAL loaded image and PPC dispatch table. No host-generated
// expected offset tables, runtime patches, GPU, or running-game attachment.
#include "renderer/engine/texture_mip_layout.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_828AE188);
extern "C" PPC_FUNC(__imp__sub_82863F68);
extern "C" PPC_FUNC(__imp__sub_828AFB00);

static void testTextureMipLayout(PPCContext& ctx) {
    using DarkRecomp::TextureMipLayout;
    using DarkRecomp::getTextureMipLayout;
    auto* base = memory->base();
    constexpr uint32_t capacity = 4 * 1024 * 1024;
    struct Fixture {
        uint32_t scratch = memory->allocate(0x10000);
        uint32_t source = memory->allocate(0x100000);
        uint32_t basePixels = memory->allocate(capacity);
        uint32_t mipPixels = memory->allocate(capacity);
        ~Fixture() {
            for (auto p : {scratch, source, basePixels, mipPixels})
                if (p) memory->release(p);
        }
    } fixture;
    check(fixture.scratch && fixture.source && fixture.basePixels && fixture.mipPixels,
          "Mip oracle fixture allocation failed");
    const uint32_t header = fixture.scratch, output = header + 256;
    const uint32_t rectangle = header + 512, point = header + 544;
    // Use a private guest stack: direct calls with >8 arguments must not
    // overwrite the native_tests caller's guest parameter-save area.
    auto context = [&] {
        auto call = ctx;
        call.r1.u64 = fixture.scratch + 0xf000;
        return call;
    };
    struct Oracle {
        uint32_t rowPitch, faceStride, totalBytes, packedBytes;
        uint32_t width, height, depth, address, relativeOffset;
    };
    auto query = [&](uint32_t face, uint32_t mip) {
        auto call = context();
        call.r3.u64 = header; call.r4.u64 = face; call.r5.u64 = mip;
        __imp__sub_828AE188(call, base);
        const uint32_t relative = call.r3.u32;
        call = context();
        call.r3.u64 = header; call.r4.u64 = face; call.r5.u64 = mip;
        call.r6.u64 = output; call.r7.u64 = output + 4;
        call.r8.u64 = output + 8; call.r9.u64 = output + 12;
        call.r10.u64 = output + 16;
        memory->write32(call.r1.u32 + 84, output + 20);
        memory->write32(call.r1.u32 + 92, output + 24);
        memory->write32(call.r1.u32 + 100, output + 28);
        __imp__sub_82863F68(call, base);
        return Oracle{memory->read32(output), memory->read32(output + 4),
                      memory->read32(output + 8), memory->read32(output + 12),
                      memory->read32(output + 16), memory->read32(output + 20),
                      memory->read32(output + 24), memory->read32(output + 28), relative};
    };
    auto value = [](uint32_t seed, uint32_t face, uint32_t mip,
                    uint32_t x, uint32_t y, uint32_t lane) {
        return uint8_t(seed * 19 + face * 37 + mip * 53 + x * 7 + y * 11 + lane * 23);
    };
    uint32_t caseCount = 0, levelCount = 0;
    auto run = [&](uint32_t width, uint32_t height, uint32_t format,
                   bool cube, bool packed, uint32_t extraPitchTiles, bool tiled = true) {
        ++caseCount;
        const uint32_t block = format == 2 || format == 6 ? 1 : 4;
        const uint32_t bytes = format == 2 ? 1 : format == 6 ? 4 : format == 18 ? 8 : 16;
        // Table actually read by 82863F68/82863848 in the original image.
        check(base[0x8209caa9u + 2 * format] == bytes * 8 / (block * block),
              "Original texture format table is absent or unexpected");
        const uint32_t alignment = 32 * block;
        const uint32_t pitch = (width + alignment - 1) / alignment * alignment +
                               extraPitchTiles * alignment;
        const uint32_t endian = format == 2 ? 0 : format == 6 ? 2 : 1;
        uint32_t fetch[6]{(tiled ? 0x80000002u : 2u) | ((pitch / 32) << 22),
                          fixture.basePixels | format | (endian << 6),
                          (width - 1) | ((height - 1) << 13) | (cube ? 0x14000000u : 0),
                          0x1414, 0x3c0,
                          fixture.mipPixels | (cube ? 0x600u : 0x200u) | (packed ? 0x800u : 0)};
        std::memset(base + header, 0, 64);
        memory->write32(header, 0x00200003);
        for (unsigned i = 0; i < 6; ++i) memory->write32(header + 28 + i * 4, fetch[i]);
        std::memset(base + fixture.basePixels, 0xcd, capacity);
        std::memset(base + fixture.mipPixels, 0xcd, capacity);
        struct Pending { TextureMipLayout layout; uint32_t face, mip; };
        std::vector<Pending> pending;
        const uint32_t faces = cube ? 6 : 1;
        for (uint32_t mip = 0, w = width, h = height;; ++mip,
                      w = (std::max)(1u, w / 2), h = (std::max)(1u, h / 2)) {
            for (uint32_t face = 0; face < faces; ++face) {
                ++levelCount;
                TextureMipLayout layout;
                const auto error = getTextureMipLayout(fetch, face, mip, layout);
                if (error) std::fprintf(stderr, "Mip fixture %ux%u fmt=%u cube=%u packed=%u face=%u mip=%u: %s\n",
                    width, height, format, cube, packed, face, mip, error);
                check(!error, "Native mip layout rejected oracle fixture");
                const Oracle oracle = query(face, mip);
                // 82863F68's pitch is bytes per TEXEL row (BC formats use
                // fractional texel sizes); multiply by block height first.
                const uint32_t blockRow = oracle.rowPitch * block;
                check(blockRow && blockRow % bytes == 0,
                      "Original mip query returned an invalid block row pitch");
                const uint32_t originX = (oracle.packedBytes % blockRow) / bytes;
                const uint32_t originY = oracle.packedBytes / blockRow;
                const uint32_t allocation = oracle.address - oracle.relativeOffset;
                const uint32_t surfaceAddress = oracle.address - oracle.packedBytes;
                const bool matches = layout.originalOffsetBytes == oracle.relativeOffset &&
                    layout.allocationAddress == allocation &&
                    layout.usesMipAddress == (allocation == fixture.mipPixels) &&
                    layout.surfaceOffsetBytes == oracle.relativeOffset - oracle.packedBytes &&
                    layout.rowPitchBytes == blockRow && layout.pitchBlocks == blockRow / bytes &&
                    layout.faceStrideBytes == oracle.faceStride && oracle.totalBytes == oracle.faceStride &&
                    layout.packedOffsetBytes == oracle.packedBytes &&
                    layout.originBlockX == originX && layout.originBlockY == originY &&
                    layout.width == oracle.width && layout.height == oracle.height && oracle.depth == 1 &&
                    layout.width == w && layout.height == h;
                if (!matches) std::fprintf(stderr,
                    "Mip mismatch %ux%u fmt=%u cube=%u packed=%u face=%u mip=%u "
                    "host offset=%08X pitch=%u stride=%u packed=%u original offset=%08X pitch=%u stride=%u packed=%u\n",
                    width, height, format, cube, packed, face, mip,
                    layout.originalOffsetBytes, layout.rowPitchBytes, layout.faceStrideBytes, layout.packedOffsetBytes,
                    oracle.relativeOffset, blockRow, oracle.faceStride, oracle.packedBytes);
                check(matches, "Native mip layout disagrees with original resource helpers");
                check((allocation == fixture.basePixels || allocation == fixture.mipPixels) &&
                      surfaceAddress >= allocation &&
                      uint64_t(surfaceAddress) + oracle.faceStride <= uint64_t(allocation) + capacity,
                      "Original mip oracle escaped destination fixture");
                const uint32_t bw = (oracle.width + block - 1) / block;
                const uint32_t bh = (oracle.height + block - 1) / block;
                check(uint64_t(bw) * bh * bytes <= 0x100000, "Mip source fixture too small");
                for (uint32_t y = 0; y < bh; ++y) for (uint32_t x = 0; x < bw; ++x)
                    for (uint32_t lane = 0; lane < bytes; ++lane)
                        base[fixture.source + (y * bw + x) * bytes + lane] = value(caseCount, face, mip, x, y, lane);
                // Destination and packed coordinates come ONLY from the
                // original query, not from any member of the host layout.
                memory->write32(point, originX); memory->write32(point + 4, originY);
                memory->write32(rectangle, 0); memory->write32(rectangle + 4, 0);
                memory->write32(rectangle + 8, bw); memory->write32(rectangle + 12, bh);
                auto call = context();
                call.r3.u64 = surfaceAddress; call.r4.u64 = blockRow / bytes;
                call.r5.u64 = originY + bh; call.r6.u64 = point;
                call.r7.u64 = fixture.source; call.r8.u64 = bw * bytes;
                call.r9.u64 = rectangle; call.r10.u64 = bytes;
                if (tiled) __imp__sub_828AFB00(call, base);
                else for (uint32_t y = 0; y < bh; ++y)
                    std::memcpy(base + surfaceAddress + (originY + y) * blockRow + originX * bytes,
                                base + fixture.source + y * bw * bytes, bw * bytes);
                pending.push_back({layout, face, mip});
            }
            if (w == 1 && h == 1) break;
        }
        // Read after tiling the ENTIRE resource: accidental face/level/tail
        // overlap cannot be hidden by validating immediately after each write.
        for (const auto& entry : pending) {
            const auto& layout = entry.layout;
            for (uint32_t y = 0; y < layout.blocksHigh; ++y)
                for (uint32_t x = 0; x < layout.blocksWide; ++x) {
                    const uint32_t offset = tiled ? worldTextureTiledOffset(x + layout.originBlockX,
                        y + layout.originBlockY, layout.pitchBlocks, layout.bytesPerBlock) :
                        (y + layout.originBlockY) * layout.rowPitchBytes + (x + layout.originBlockX) * layout.bytesPerBlock;
                    check(uint64_t(offset) + layout.bytesPerBlock <= layout.faceStrideBytes,
                          "Native mip tiled read escaped padded face");
                    for (uint32_t lane = 0; lane < layout.bytesPerBlock; ++lane)
                        check(base[layout.allocationAddress + layout.surfaceOffsetBytes + offset + lane] ==
                              value(caseCount, entry.face, entry.mip, x, y, lane),
                              "Native mip read differs from original-queried/original-tiled bytes");
                }
        }
        struct ProtectedSpan {
            uint8_t* address{}; DWORD old{};
            explicit ProtectedSpan(uint8_t* p) : address(p) {
                if(address)check(VirtualProtect(address,4096,PAGE_NOACCESS,&old)!=0,"Mip fixture protection failed");
            }
            ~ProtectedSpan() {if(address) {DWORD ignored{};VirtualProtect(address,4096,old,&ignored);}}
        } unusedPadding(!tiled && !packed && !cube && format==6 && width==64 && height==33 ?
                        base+fixture.basePixels+0x3000 : nullptr);
        // This resource's original stride is 16 KiB, but the last used base
        // byte is at 0x20ff. An unreadable padding page must not reject it.
        DarkRecomp::ColorImage image;
        check(!decodeWorldTextureImage(base,header,image) && image.authoredMips &&
              image.width==width && image.height==height && image.faces==faces &&
              image.mips.size()+1==std::bit_width((std::max)(width,height)),
              "Original authored mip resource failed complete native decoding");
        size_t ownedBytes=0;
        for (uint32_t mip=0,w=width,h=height;;++mip,w=(std::max)(1u,w/2),h=(std::max)(1u,h/2)) {
            const auto& pixels=mip?image.mips[mip-1]:image.pixels;
            check(pixels.size()==size_t(w)*h*faces*4,"Decoded authored mip has incorrect dimensions/face count");
            ownedBytes+=pixels.size();
            if(format==2 || format==6)for(uint32_t face=0;face<faces;++face)
                for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
                    const auto* pixel=pixels.data()+((size_t(face)*h+y)*w+x)*4;
                    // Resource selectors 2,1,0,1-constant; RGBA endian mode
                    // reverses each source word before applying selectors.
                    const uint8_t expected[4]{format==6?value(caseCount,face,mip,x,y,1):uint8_t(0),
                        format==6?value(caseCount,face,mip,x,y,2):uint8_t(0),
                        value(caseCount,face,mip,x,y,format==6?3:0),255};
                    check(!std::memcmp(pixel,expected,4),"Authored mip decode lost original level/face/swizzle/endian pixels");
                }
            if(w==1 && h==1)break;
        }
        check(image.bytes()==ownedBytes,"Authored mip cache budget omitted lower levels");
        if(unusedPadding.address) {
            ProtectedSpan missingMip(base+fixture.mipPixels);
            DarkRecomp::ColorImage unchanged;unchanged.width=99;unchanged.pixels={1,2,3,4};
            check(decodeWorldTextureImage(base,header,unchanged)!=nullptr && unchanged.width==99 &&
                  unchanged.pixels==std::vector<uint8_t>({1,2,3,4}) && unchanged.mips.empty(),
                  "Unreadable authored mip silently fell back or partially replaced output");
            DarkRecomp::ColorImage tail;
            check(!decodeWorldTextureImage(base,header,tail,2) && tail.firstMip==2 && tail.pixels.empty() &&
                  tail.mips[0].empty() && tail.mips[1]==image.mips[1] && tail.mips.back()==image.mips.back(),
                  "Resident-tail recovery read unavailable leading levels or changed original indices");
            std::puts("Authored mip reads exclude inaccessible padding and reject inaccessible texels atomically.");
        }
    };

    // Exact descriptor used by the parent's forced-LOD GPU regression. These
    // small addresses are arithmetic-only: the query never reads pixel memory.
    // All expectations come from the original helper; no (mip-1)*4096 oracle.
    {
        const uint32_t fetch[6]{0x80800002, 0x1006, 63 | (63 << 13), 0xd10, 6 << 6, 0x6200};
        std::memset(base + header, 0, 64);
        memory->write32(header, 0x00200003);
        for (unsigned i = 0; i < 6; ++i) memory->write32(header + 28 + i * 4, fetch[i]);
        ++caseCount;
        for (uint32_t mip = 0; mip < 7; ++mip) {
            ++levelCount;
            const auto oracle = query(0, mip);
            TextureMipLayout layout;
            check(!getTextureMipLayout(fetch, 0, mip, layout) &&
                  layout.originalOffsetBytes == oracle.relativeOffset &&
                  layout.allocationAddress + layout.surfaceOffsetBytes == oracle.address &&
                  layout.rowPitchBytes == oracle.rowPitch &&
                  layout.faceStrideBytes == oracle.faceStride &&
                  layout.width == oracle.width && layout.height == oracle.height &&
                  !layout.packed && !oracle.packedBytes,
                  "Parent GPU mip descriptor disagrees with original resource helpers");
            std::printf("Original 64x64 RGBA mip%u: address=%08X relative=%08X size=%ux%u pitchBytes=%u stride=%u\n",
                        mip, oracle.address, oracle.relativeOffset, oracle.width, oracle.height,
                        oracle.rowPitch, oracle.faceStride);
        }
    }

    for (unsigned format : {2, 6, 18, 20, 49}) for (bool packed : {false, true}) {
        for (const auto size : std::initializer_list<std::array<uint32_t, 2>>{
                {1, 1}, {4, 4}, {8, 16}, {16, 8}, {16, 16}, {16, 256}, {256, 16},
                {32, 32}, {64, 32}, {32, 64}, {64, 64}, {128, 128}, {256, 256},
                {324, 18}, {18, 324}, {300, 180}, {180, 300}, {52, 100}, {100, 52},
                {17, 65}, {65, 17}, {1, 257}, {257, 1}, {3, 7}, {7, 3}})
            run(size[0], size[1], format, false, packed, 0);
        for (unsigned dimension : {1, 4, 8, 16, 32, 64, 128, 256})
            run(dimension, dimension, format, true, packed, 0);
        // Descriptor base pitch must affect mip0 only, including cube stride.
        run(64, 64, format, true, packed, 1);
        run(324, 18, format, false, packed, 1);
        for (const auto size : std::initializer_list<std::array<uint32_t, 2>>{
                {1, 1}, {16, 8}, {64, 64}, {324, 18}, {18, 324}, {300, 180}})
            run(size[0], size[1], format, false, packed, 0, false);
        run(64, 64, format, true, packed, 0, false);
    }

    run(64,33,6,false,false,0,false);

    // Contract failures, not synthetic tests of the offset implementation.
    uint32_t fetch[6]{0x80800002, fixture.basePixels | 6, 63 | (63 << 13), 0, 0x180,
                      fixture.mipPixels | 0xa00};
    TextureMipLayout result;
    result.width = 0xdeadbeef;
    auto rejects = [&](const uint32_t (&bad)[6], uint32_t face, uint32_t mip) {
        check(getTextureMipLayout(bad, face, mip, result) != nullptr && result.width == 0xdeadbeef,
              "Invalid mip descriptor accepted or output changed on failure");
    };
    rejects(fetch, 1, 0); rejects(fetch, 0, 7); rejects(fetch, 0, 32);
    uint32_t bad[6];
    std::memcpy(bad, fetch, sizeof(bad)); bad[0] &= ~3u; rejects(bad, 0, 0);
    std::memcpy(bad, fetch, sizeof(bad)); bad[3] |= 0x80000000u; rejects(bad, 0, 1);
    std::memcpy(bad, fetch, sizeof(bad)); bad[1] |= 0x400; rejects(bad, 0, 1);
    std::memcpy(bad, fetch, sizeof(bad)); bad[1] &= 0xfff; rejects(bad, 0, 0);
    std::memcpy(bad, fetch, sizeof(bad)); bad[5] &= 0xfff; rejects(bad, 0, 1);
    std::memcpy(bad, fetch, sizeof(bad)); bad[5] = (bad[5] & ~0x600u) | 0x400; rejects(bad, 0, 1);
    std::memcpy(bad, fetch, sizeof(bad)); bad[1] = (bad[1] & ~63u) | 7; rejects(bad, 0, 1);
    std::memcpy(bad, fetch, sizeof(bad)); bad[0] = 0x80400002; rejects(bad, 0, 0);
    std::memcpy(bad, fetch, sizeof(bad)); bad[1] = 0xfffff006; rejects(bad, 0, 0);
    std::printf("Texture mip layout: %u descriptors, %u face/level queries against original helpers and tiler.\n",
                caseCount, levelCount);
}
