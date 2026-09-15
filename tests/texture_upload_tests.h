#pragma once

// Upload transaction tests run by NativeTests.
// Ordinary fixtures use a private 64 KiB vector as the decoder base; no loaded
// game, dispatch table, hooks, GPU, or authored-mip decoder input is needed.
// Optionally pass (base, scratchAddress) to use a caller-owned, writable 64 KiB
// guest allocation instead. This function overwrites that scratch span only.
#include "renderer/engine/texture_upload.h"
#include "renderer/engine/texture_mip_layout.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace TextureUploadTests {
using DarkRecomp::Native::TextureUpload;
inline void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::vector<uint8_t> memory;
    uint8_t* base;
    uint32_t origin, resource, image, data;
    Fixture(uint8_t* guestBase, uint32_t scratch)
        : memory(scratch ? 0 : 65536), base(scratch ? guestBase : memory.data()),
          origin(scratch), resource(scratch + 256), image(scratch + 512), data(scratch + 4096) {
        expect(base && uint64_t(scratch) + 65536 <= 0x100000000ull,
               "Texture upload fixture requires a valid 64 KiB span");
        std::memset(base + origin, 0, 65536);
    }
    void be(uint32_t address, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) base[address + i] = uint8_t(value >> (24 - i * 8));
    }
    void le(uint32_t address, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) base[address + i] = uint8_t(value >> (i * 8));
    }
    void descriptor(unsigned width, unsigned height, unsigned levels, unsigned faces = 1) {
        std::memset(base + resource, 0, 64);
        const std::array<uint32_t, 6> fetch{
            2u, 6u, (width - 1) | ((height - 1) << 13) | (faces == 6 ? 5u << 26 : 0),
            0x1414u, (levels - 1) << 6, faces == 6 ? 0x600u : 0x200u};
        for (unsigned i = 0; i < 6; ++i) be(resource + 28 + i * 4, fetch[i]);
    }
    void header(unsigned width, unsigned height, unsigned storage, unsigned pitch,
                unsigned bytesPerPixel, unsigned format, unsigned flags) {
        std::memset(base + image, 0, 256);
        be(image, 0x82097610); be(image + 8, data); be(image + 12, storage);
        be(image + 16, width); be(image + 20, height); be(image + 24, pitch);
        be(image + 28, bytesPerPixel); be(image + 32, format); be(image + 40, flags);
    }
    uint32_t argb(unsigned w, unsigned h, uint8_t seed, bool padded = false) {
        const unsigned pitch = w * 4 + (padded ? 4 : 0);
        expect(uint64_t(pitch) * h <= 4096, "ARGB fixture escaped its 4 KiB data range");
        header(w, h, pitch * h, pitch, 4, 0x800, 0x814);
        std::memset(base + data, 0xee, 4096);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            auto* p = base + data + y * pitch + x * 4;
            p[0] = uint8_t(seed + 1); p[1] = uint8_t(seed + 2);
            p[2] = uint8_t(seed + 3); p[3] = uint8_t(seed + 4);
        }
        return data;
    }
    uint32_t bc1(unsigned w, unsigned h, unsigned color = 0) {
        const unsigned bytes = ((w + 3) / 4) * ((h + 3) / 4) * 8;
        expect(bytes + 16 <= 4096, "BC1 fixture escaped its 4 KiB data range");
        header(w, h, 16 + bytes, 0, 4, 0x800, 0x5014);
        std::memset(base + data, 0, 4096);
        le(data, 0); le(data + 4, bytes); le(data + 8, 0); le(data + 12, 16);
        const uint16_t endpoint[] = {0xf800, 0x07e0, 0x001f};
        for (unsigned offset = 16; offset < bytes + 16; offset += 8) {
            base[data + offset] = uint8_t(endpoint[color % 3]);
            base[data + offset + 1] = uint8_t(endpoint[color % 3] >> 8);
        }
        return data + 16;
    }
    void skipped(unsigned w, unsigned h, bool compressed = true) {
        // Valid dimensions but deliberately unusable storage. A metadata-only
        // level can borrow data+16 from an entirely different resident image.
        header(w, h, 1, 0, 4, 0x800, compressed ? 0x5214 : 0xa14);
        be(image + 8, 0xfffffff0u);
    }
};

inline void solid(const std::vector<uint8_t>& pixels, size_t offset, size_t count,
                  std::array<uint8_t, 4> rgba) {
    expect(offset <= pixels.size() && count <= pixels.size() - offset,
           "Texture upload output slice is missing");
    expect(count % 4 == 0, "Texture upload RGBA size is not integral");
    for (size_t i = offset; i < offset + count; i += 4)
        for (unsigned channel = 0; channel < 4; ++channel)
            expect(pixels[i + channel] == rgba[channel], "Texture upload owned RGBA bytes differ");
}
inline std::array<uint8_t, 4> rawColor(uint8_t seed) {
    return {uint8_t(seed + 3), uint8_t(seed + 2), uint8_t(seed + 1), uint8_t(seed + 4)};
}

inline std::shared_ptr<const DarkRecomp::ColorImage> makeSeed(Fixture& f, unsigned faces,
                                                           unsigned first = 2) {
    f.descriptor(8, 8, 4, faces);
    TextureUpload initial(30, 4, first, faces);
    for (unsigned face = 0; face < faces; ++face) for (unsigned mip = 0; mip < 4; ++mip) {
        if (mip < first) f.skipped(8 >> mip, 8 >> mip);
        else f.argb(8 >> mip, 8 >> mip, uint8_t(50 + mip + face));
        expect(initial.record(f.base, f.resource, f.image, f.data, mip, face, mip < first),
               "Incremental seed record failed");
        if (mip >= first) expect(initial.complete(mip, face), "Incremental seed completion failed");
    }
    auto seed = initial.finish(f.resource);
    expect(seed && seed->valid() && seed->authoredMips && seed->firstMip == first,
           "Incremental seed construction failed");
    return seed;
}

inline void incremental(uint8_t* base, uint32_t scratch) {
    // Cache-miss recovery decodes native fetch formats; CPU uploads decode
    // CImage formats. Both produce owned RGBA, but their codec IDs differ.
    for (unsigned format : {6u, 2u}) {
        Fixture f(base, scratch);
        const uint32_t fetch[]{2u | (1u << 22), (f.origin + 0x8000) | format,
            7u | (7u << 13), format == 6 ? 0xd10u : 0x2dau,
            3u << 6, (f.origin + 0xa000) | 0x200u};
        for (unsigned i = 0; i < 6; ++i) f.be(f.resource + 28 + i * 4, fetch[i]);
        for (unsigned mip = 2; mip < 4; ++mip) {
            DarkRecomp::TextureMipLayout layout;
            expect(!DarkRecomp::getTextureMipLayout(fetch, 0, mip, layout), "Recovered-tail layout failed");
            for (unsigned y = 0; y < layout.height; ++y) for (unsigned x = 0; x < layout.width; ++x) {
                const uint64_t address = uint64_t(layout.allocationAddress) + layout.surfaceOffsetBytes +
                    y * layout.rowPitchBytes + x * layout.bytesPerBlock;
                expect(address >= f.origin && address + layout.bytesPerBlock <= uint64_t(f.origin) + 65536,
                       "Recovered-tail fixture escaped allocation");
                if (format == 6) {
                    const uint8_t rgba[]{uint8_t(40 + mip), 60, 80, 255};
                    std::memcpy(f.base + address, rgba, 4);
                } else f.base[address] = uint8_t(100 + mip);
            }
        }
        auto seed = std::make_shared<DarkRecomp::ColorImage>();
        expect(!DarkRecomp::Native::decodeWorldTextureImage(f.base, f.resource, *seed, 2) &&
               seed->valid() && seed->firstMip == 2 && seed->sourceCodec == format,
               "Native resource tail recovery failed");
        for (unsigned mip = 2; mip < 4; ++mip)
            solid(seed->mips[mip - 1], 0, seed->mips[mip - 1].size(), format == 6 ?
                std::array<uint8_t, 4>{uint8_t(40 + mip), 60, 80, 255} :
                std::array<uint8_t, 4>{255, 255, 255, uint8_t(100 + mip)});
        const auto before = *seed;
        TextureUpload update(32, 4, 0, 1, seed, 2);
        for (unsigned mip = 0; mip < 2; ++mip) {
            const unsigned side = 8 >> mip;
            if (format == 6) f.argb(side, side, uint8_t(10 + mip));
            else {
                f.header(side, side, side * side, side, 1, 0x40000, 0x814);
                std::memset(f.base + f.data, 120 + mip, side * side);
            }
            expect(update.record(f.base, f.resource, f.image, f.data, mip, 0, false) && update.complete(mip, 0),
                   "Recovered native tail rejected compatible CImage prefix");
        }
        auto result = update.finish(f.resource);
        expect(result && result->valid() && result->firstMip == 0 &&
               result->mips[1] == before.mips[1] && result->mips[2] == before.mips[2] &&
               seed->pixels == before.pixels && seed->mips == before.mips && seed->firstMip == 2,
               "Recovered tail merge lost pixels or changed its old generation");
        for (unsigned mip = 0; mip < 2; ++mip) {
            const auto& pixels = mip ? result->mips[0] : result->pixels;
            solid(pixels, 0, pixels.size(), format == 6 ? rawColor(uint8_t(10 + mip)) :
                std::array<uint8_t, 4>{255, 255, 255, uint8_t(120 + mip)});
        }
    }
    std::puts("CPUUploadRecovery: native RGBA/L8 tails merge with CImage color/alpha prefixes.");
    // A sparse firstMip=2 chain becomes full by refreshing exactly mip0,1.
    // Both 2D and cube input are face-major; tail bytes retain original indices.
    for (unsigned faces : {1u, 6u}) {
        Fixture f(base, scratch); auto seed = makeSeed(f, faces);
        const auto before = *seed;
        TextureUpload update(30, 4, 0, faces, seed, 2);
        expect(!update.error() && update.ownedBytes() == seed->bytes(), "Seed bytes not budgeted");
        for (unsigned face = 0; face < faces; ++face) for (unsigned mip = 0; mip < 2; ++mip) {
            f.argb(8 >> mip, 8 >> mip, uint8_t(10 + face + mip));
            expect(update.record(f.base, f.resource, f.image, f.data, mip, face, false) && update.complete(mip, face),
                   "Fresh incremental prefix record or completion failed");
        }
        expect(update.ownedBytes() == seed->bytes() + uint64_t(faces) * (64 + 16) * 4,
               "Incremental budget omitted seed or fresh pending bytes");
        auto result = update.finish(f.resource);
        expect(result && result->valid() && result->authoredMips && result->firstMip == 0 &&
               result->width == 8 && result->height == 8 && result->faces == faces &&
               result->sourceCodec == seed->sourceCodec && result->mips.size() == 3 &&
               result->mips[1] == before.mips[1] && result->mips[2] == before.mips[2] &&
               update.ownedBytes() == 0, "Incremental prefix merge lost logical shape or seed tail");
        for (unsigned mip = 0; mip < 2; ++mip) {
            const auto& level = mip ? result->mips[mip - 1] : result->pixels;
            const size_t faceBytes = size_t(8 >> mip) * (8 >> mip) * 4;
            for (unsigned face = 0; face < faces; ++face)
                solid(level, face * faceBytes, faceBytes, rawColor(uint8_t(10 + face + mip)));
        }
        expect(result.get() != seed.get() && result->mips[1].data() != seed->mips[1].data(),
               "Merged result aliases immutable seed pixels");
        result->mips[1][0] ^= 1;
        expect(seed->pixels == before.pixels && seed->mips == before.mips && seed->firstMip == 2 && seed->valid(),
               "Successful prefix update mutated its old seed");
    }
    // Even a fully resident seed cannot stand in for a missing fresh record or
    // completion witness. Cover one missing actual face and one missing copy.
    for (unsigned first : {0u, 2u}) for (unsigned missing : {0u, 1u}) {
        Fixture f(base, scratch); auto seed = makeSeed(f, 6, first); const auto before = *seed;
        TextureUpload update(30, 4, 0, 6, seed, 2);
        for (unsigned face = 0; face < 6; ++face) for (unsigned mip = 0; mip < 2; ++mip) {
            if (missing == 0 && face == 5 && mip == 1) continue;
            f.argb(8 >> mip, 8 >> mip, uint8_t(mip + face));
            expect(update.record(f.base, f.resource, f.image, f.data, mip, face, false), "Incomplete prefix fixture failed");
            if (!(missing == 1 && face == 5 && mip == 1))
                expect(update.complete(mip, face), "Incomplete prefix completion fixture failed");
        }
        expect(!update.finish(f.resource) && update.error() && update.ownedBytes() == 0 &&
               seed->pixels == before.pixels && seed->mips == before.mips && seed->firstMip == first && seed->valid(),
               "Seed fulfilled fresh witnesses or was mutated by failed prefix update");
    }
    // Invalid mode combinations, gaps in the seed tail, non-authored/invalid
    // seeds, and inconsistent counts/faces must fail without reading guest data.
    for (unsigned mode = 0; mode < 9; ++mode) {
        Fixture f(base, scratch); auto seed = makeSeed(f, 1);
        auto candidate = std::make_shared<DarkRecomp::ColorImage>(*seed);
        if (mode == 0) candidate.reset();
        if (mode == 1) candidate->mips[2].clear();
        if (mode == 2) candidate->authoredMips = false;
        const unsigned end = mode == 3 ? 1 : mode == 4 ? 5 : mode == 5 ? 0 : 2;
        TextureUpload update(30, mode == 6 ? 3 : 4, mode == 7 ? 1 : 0,
                             mode == 8 ? 6 : 1, candidate, end);
        expect(update.error() && update.ownedBytes() == 0 && !update.finish(f.resource),
               "Invalid incremental seed/range silently became a full reset");
    }
    // Tail levels cannot be recorded or completed; prefix updates never skip.
    // Resource dimensions must still match the seed.
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f(base, scratch); auto seed = makeSeed(f, 1); const auto before = *seed;
        TextureUpload update(30, 4, 0, 1, seed, 2);
        uint32_t pixels = f.argb(8, 8, 1);
        if (mode == 0) {
            f.argb(2, 2, 1);
            expect(!update.record(f.base, f.resource, f.image, f.data, 2, 0, false), "Seed tail accepted a fresh record");
        } else if (mode == 1) expect(!update.complete(2, 0), "Seed tail accepted a completion");
        else {
            if (mode == 3) f.descriptor(8, 4, 4);
            expect(!update.record(f.base, f.resource, f.image, pixels, 0, 0, mode == 2),
                   "Skipped prefix or changed dimensions accepted");
        }
        expect(!update.finish(f.resource) && seed->pixels == before.pixels && seed->mips == before.mips,
               "Rejected incremental operation published or mutated seed");
    }
    // The immutable tail is RGBA even if it came from a different CImage
    // codec. Only the new slots must agree with one another.
    {
        Fixture f(base, scratch);auto seed=makeSeed(f,1);const auto before=*seed;
        TextureUpload update(30,4,0,1,seed,2);
        for(unsigned mip=0;mip<2;++mip) {
            const auto pixels=f.bc1(8>>mip,8>>mip,mip);
            expect(update.record(f.base,f.resource,f.image,pixels,mip,0,false) && update.complete(mip,0),
                   "RGBA seed rejected a freshly decoded prefix codec");
        }
        auto result=update.finish(f.resource);
        expect(result && result->valid() && result->sourceCodec==0 && result->firstMip==0 &&
               result->mips[1]==before.mips[1] && result->mips[2]==before.mips[2] && seed->mips==before.mips,
               "Mixed-provenance update lost immutable RGBA tail");
        solid(result->pixels,0,result->pixels.size(),{255,0,0,255});
        solid(result->mips[0],0,result->mips[0].size(),{0,255,0,255});
    }
    // Updating the complete chain is valid too, but every level must be fresh.
    {
        Fixture f(base, scratch); auto seed = makeSeed(f, 1);
        TextureUpload update(30, 4, 0, 1, seed, 4);
        for (unsigned mip = 0; mip < 4; ++mip) {
            f.argb(8 >> mip, 8 >> mip, uint8_t(mip));
            expect(update.record(f.base, f.resource, f.image, f.data, mip, 0, false) && update.complete(mip, 0),
                   "Full incremental range rejected");
        }
        auto result = update.finish(f.resource);
        expect(result && result->valid() && result->firstMip == 0 && result->mips[2] != seed->mips[2],
               "Full incremental range reused old tail");
    }
    // A cache miss may replace the whole resource without a seed, since no
    // unchanged tail is needed. Missing any fresh completion still rejects.
    for (bool omitLastCopy : {false, true}) {
        Fixture f(base, scratch); f.descriptor(8, 8, 4);
        TextureUpload replacement(31, 4, 0, 1, {}, 4);
        expect(!replacement.error(), "Seedless full replacement rejected at construction");
        for (unsigned mip = 0; mip < 4; ++mip) {
            f.argb(8 >> mip, 8 >> mip, uint8_t(20 + mip));
            expect(replacement.record(f.base, f.resource, f.image, f.data, mip, 0, false),
                   "Seedless replacement record failed");
            if (!(omitLastCopy && mip == 3))
                expect(replacement.complete(mip, 0), "Seedless replacement completion failed");
        }
        auto result = replacement.finish(f.resource);
        if (omitLastCopy) expect(!result && replacement.error(), "Seedless replacement lacked a fresh witness");
        else {
            expect(result && result->valid() && result->authoredMips && result->firstMip == 0 &&
                   result->mips.size() == 3, "Seedless full replacement did not publish a complete chain");
            solid(result->pixels, 0, 256, rawColor(20));
            solid(result->mips[2], 0, 4, rawColor(23));
        }
    }
    {
        Fixture f(base, scratch); f.descriptor(8, 8, 4);
        // The parent skips constructing an incremental transaction for count=0.
        // Even if constructed with the initial-mode sentinel, no actual calls
        // means no resource binding/witnesses and therefore no publication.
        TextureUpload empty(31, 4, 0, 1, {}, 0);
        expect(!empty.finish(f.resource), "Zero-copy upload published an image");
    }
}

inline void run(uint8_t* base, uint32_t scratch) {
    // Resource maxLOD is intentionally shorter than the natural 8x4 chain.
    {
        Fixture f(base, scratch); f.descriptor(8, 4, 3);
        TextureUpload upload(17, 3, 0, 1);
        for (unsigned mip = 0; mip < 3; ++mip) {
            const auto pixels = f.argb(8 >> mip, 4 >> mip, uint8_t(10 + mip), true);
            expect(upload.record(f.base, f.resource, f.image, pixels, mip, 0, false), "ARGB record failed");
            // Guest lifetime ends immediately after observation; changing it
            // before completion must not change the retained snapshot.
            std::memset(f.base + f.data, 0xdd, 4096);
            expect(upload.complete(mip, 0), "ARGB copy completion failed");
        }
        auto result = upload.finish(f.resource);
        expect(result && result->width == 8 && result->height == 4 && result->faces == 1 &&
               result->authoredMips && result->firstMip == 0 && result->sourceCodec == 0x800 &&
               result->mips.size() == 2 && result->bytes() == (32 + 8 + 2) * 4,
               "ARGB resource-relative chain contract failed");
        solid(result->pixels, 0, 128, rawColor(10));
        solid(result->mips[0], 0, 32, rawColor(11)); solid(result->mips[1], 0, 8, rawColor(12));
        expect(upload.ownedBytes() == 0 && !upload.error() && upload.id() == 17,
               "Finish retained pending bytes or lost transaction identity");
        expect(!upload.finish(f.resource) && !upload.complete(0, 0) &&
               !upload.record(f.base, f.resource, f.image, f.data, 0, 0, false),
               "Finished upload accepted more events");
        TextureUpload reused(17, 1, 0, 1); f.descriptor(1, 1, 1); f.argb(1, 1, 90);
        expect(reused.record(f.base, f.resource, f.image, f.data, 0, 0, false) && reused.complete(0, 0),
               "Reused texture ID inherited transaction state");
        auto next = reused.finish(f.resource);
        expect(next && next.get() != result.get(), "Texture generation shared mutable output");
        solid(result->pixels, 0, 128, rawColor(10));
    }
    // 2D NPOT chain exercises max(1, logicalDimension >> resourceMip).
    {
        Fixture f(base, scratch); f.descriptor(7, 3, 3); TextureUpload upload(1, 3, 0, 1);
        for (unsigned mip = 0; mip < 3; ++mip) {
            const unsigned w = (std::max)(1u, 7u >> mip), h = (std::max)(1u, 3u >> mip);
            f.argb(w, h, uint8_t(mip));
            expect(upload.record(f.base, f.resource, f.image, f.data, mip, 0, false) && upload.complete(mip, 0),
                   "NPOT resource-relative record failed");
        }
        auto result = upload.finish(f.resource);
        expect(result && result->mips[0].size() == 12 && result->mips[1].size() == 4,
               "NPOT lower dimensions were not clamped to one");
    }
    // Cubes arrive face-major, not mip-major. Cover full and missing-base chains
    // with BC1; every output level remains face-major with exact color bytes.
    for (const unsigned first : {0u, 2u}) {
        Fixture f(base, scratch); f.descriptor(8, 8, 4, 6); TextureUpload upload(2, 4, first, 6);
        for (unsigned face = 0; face < 6; ++face) for (unsigned mip = 0; mip < 4; ++mip) {
            const unsigned side = 8 >> mip;
            if (mip < first) {
                f.skipped(side, side);
                expect(upload.record(f.base, f.resource, f.image, f.data + 16, mip, face, true),
                       "Skipped cube metadata decoded borrowed bytes");
            } else {
                const auto pixels = f.bc1(side, side, face + mip);
                expect(upload.record(f.base, f.resource, f.image, pixels, mip, face, false) && upload.complete(mip, face),
                       "Face-major cube record failed");
            }
        }
        auto result = upload.finish(f.resource);
        expect(result && result->width == 8 && result->height == 8 && result->faces == 6 &&
               result->sourceCodec == 0 && result->authoredMips && result->firstMip == first &&
               result->mips.size() == 3 && result->pixels.empty() == (first != 0),
               "Cube residency rebased logical dimensions or mip indices");
        for (unsigned mip = 0; mip < 4; ++mip) {
            const auto& level = mip ? result->mips[mip - 1] : result->pixels;
            if (mip < first) { expect(level.empty(), "Skipped level manufactured pixels"); continue; }
            const size_t faceBytes = size_t(8 >> mip) * (8 >> mip) * 4;
            expect(level.size() == faceBytes * 6, "Cube face payload size mismatch");
            for (unsigned face = 0; face < 6; ++face) {
                std::array<uint8_t, 4> color{0, 0, 0, 255}; color[(face + mip) % 3] = 255;
                solid(level, face * faceBytes, faceBytes, color);
            }
        }
    }
    // Missing base on 2D, plus identical skipped and actual duplicates.
    {
        Fixture f(base, scratch); f.descriptor(8, 4, 3); TextureUpload upload(3, 3, 2, 1);
        for (unsigned mip = 0; mip < 2; ++mip) {
            f.skipped(8 >> mip, 4 >> mip, false);
            for (unsigned repeat = 0; repeat < 2; ++repeat)
                expect(upload.record(f.base, f.resource, f.image, 0, mip, 0, true), "Skipped duplicate rejected");
        }
        f.argb(2, 1, 33);
        for (unsigned repeat = 0; repeat < 2; ++repeat)
            expect(upload.record(f.base, f.resource, f.image, f.data, 2, 0, false) && upload.complete(2, 0),
                   "Identical actual duplicate rejected");
        expect(upload.ownedBytes() == 8, "Duplicate counted retained bytes twice");
        auto result = upload.finish(f.resource);
        expect(result && result->firstMip == 2 && result->pixels.empty() && result->mips.size() == 2 &&
               result->mips[0].empty() && result->mips[1].size() == 8, "Missing-base 2D chain was compacted");
        solid(result->mips[1], 0, 8, rawColor(33));
    }
    // Completion is independent from decode, including repeated invocations.
    for (unsigned mode = 0; mode < 5; ++mode) {
        Fixture f(base, scratch); f.descriptor(2, 2, 1); f.argb(2, 2, 1);
        TextureUpload upload(4, 1, 0, 1);
        if (mode == 0) expect(!upload.complete(0, 0), "Completion before record accepted");
        else {
            expect(upload.record(f.base, f.resource, f.image, f.data, 0, 0, false), "Completion fixture record failed");
            if (mode >= 2) expect(upload.complete(0, 0), "Completion fixture witness failed");
            if (mode == 2) expect(upload.record(f.base, f.resource, f.image, f.data, 0, 0, false), "Duplicate record failed");
            if (mode == 3) expect(!upload.complete(1, 0), "Out-of-range mip completion accepted");
            if (mode == 4) expect(!upload.complete(0, 1), "Out-of-range face completion accepted");
        }
        expect(!upload.finish(f.resource) && upload.error() && upload.ownedBytes() == 0,
               "Unwitnessed or invalid copy published pixels");
    }
    // Every skipped/actual face and every actual completion is mandatory.
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture f(base, scratch); f.descriptor(2, 2, 2, 6); TextureUpload upload(5, 2, 1, 6);
        for (unsigned face = 0; face < 6; ++face) {
            f.skipped(2, 2);
            if (!(mode == 0 && face == 5))
                expect(upload.record(f.base, f.resource, f.image, f.data, 0, face, true), "Skipped fixture record failed");
            f.argb(1, 1, uint8_t(face));
            if (mode == 1 && face == 5) continue;
            expect(upload.record(f.base, f.resource, f.image, f.data, 1, face, false), "Missing-face fixture record failed");
            if (!(mode == 2 && face == 5)) expect(upload.complete(1, face), "Missing-face fixture completion failed");
        }
        expect(!upload.finish(f.resource), "Incomplete cube published pixels");
    }
    // Actual metadata cannot use the raw route OR the compressed fallback.
    for (bool compressed : {false, true}) {
        Fixture f(base, scratch); f.descriptor(4, 4, 1);
        const auto pixels = compressed ? f.bc1(4, 4) : f.argb(4, 4, 1);
        f.be(f.image + 40, compressed ? 0x5214 : 0xa14);
        TextureUpload upload(6, 1, 0, 1);
        expect(!upload.record(f.base, f.resource, f.image, pixels, 0, 0, false) && !upload.finish(f.resource),
               "Actual metadata-only image accepted");
    }
    // Accessible but unowned/incorrect pointers must fail without large ranges
    // or guard-page fixtures. Storage and data are fully inside our small vector.
    for (unsigned mode = 0; mode < 6; ++mode) {
        Fixture f(base, scratch); f.descriptor(4, 4, 1);
        uint32_t pixels = mode < 3 ? f.argb(4, 4, 1) : f.bc1(4, 4);
        if (mode == 0) pixels -= 4;
        if (mode == 1) pixels += 4;
        if (mode == 2) f.be(f.image + 12, 63);
        if (mode == 3) pixels += 8;
        if (mode == 4) f.be(f.image + 12, 23);
        if (mode == 5) f.le(f.data + 4, 7);
        TextureUpload upload(7, 1, 0, 1);
        expect(!upload.record(f.base, f.resource, f.image, pixels, 0, 0, false) && upload.error(),
               "Upload accepted bytes outside the exact owned image span");
    }
    // Resource identity includes all six fetch words, on records AND finish.
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f(base, scratch); f.descriptor(2, 2, 1); f.argb(2, 2, 4);
        TextureUpload upload(8, 1, 0, 1);
        expect(upload.record(f.base, f.resource, f.image, f.data, 0, 0, false) && upload.complete(0, 0),
               "Resource change fixture failed");
        if (mode == 0) expect(!upload.record(f.base, f.resource + 64, f.image, f.data, 0, 0, false), "Wrong resource record accepted");
        if (mode >= 2) f.be(f.resource + 40, 0x1415);
        if (mode == 2) expect(!upload.record(f.base, f.resource, f.image, f.data, 0, 0, false), "Changed fetch words accepted");
        expect(!upload.finish(mode == 1 ? f.resource + 64 : f.resource), "Changed resource published pixels");
    }
    // Conflicting actual bytes, actual metadata, codec across mips, and skipped
    // metadata all poison the transaction; a subsequent good record cannot heal it.
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture f(base, scratch); f.descriptor(4, 4, 2);
        TextureUpload upload(9, 2, mode == 3 ? 1 : 0, 1);
        if (mode == 3) f.skipped(4, 4); else f.argb(4, 4, 10);
        expect(upload.record(f.base, f.resource, f.image, f.data, 0, 0, mode == 3), "Conflict fixture record failed");
        unsigned mip = 0;
        if (mode == 0) f.base[f.data] ^= 1;
        if (mode == 1) f.be(f.image + 40, 0x810); // Same decode, conflicting header.
        if (mode == 3) f.be(f.image + 24, 20);
        if (mode == 2) { f.bc1(2, 2); mip = 1; }
        const uint32_t pixels = mode == 2 ? f.data + 16 : f.data;
        expect(!upload.record(f.base, f.resource, f.image, pixels, mip, 0, mode == 3) && upload.error() &&
               !upload.complete(mip, 0) && !upload.finish(f.resource) && upload.ownedBytes() == 0,
               "Conflicting upload was not a sticky failure");
        f.argb(4, 4, 10);
        expect(!upload.record(f.base, f.resource, f.image, f.data, 0, 0, false), "Failed upload recovered implicitly");
    }
    // Constructor count must be explicit and agree exactly with maxLOD+1.
    for (unsigned mode = 0; mode < 8; ++mode) {
        Fixture f(base, scratch); f.descriptor(4, 4, 2); f.argb(4, 4, 1);
        const unsigned levels = mode == 0 ? 0 : mode == 1 ? 13 : mode == 2 ? 1 : mode == 3 ? 3 : 2;
        TextureUpload upload(mode == 7 ? 0 : 10, levels, mode == 4 ? 2 : 0, mode == 5 ? 2 : mode == 6 ? 6 : 1);
        expect(!upload.record(f.base, f.resource, f.image, f.data, 0, 0, false) && !upload.finish(f.resource),
               "Invalid or ambiguous request was normalized implicitly");
    }
    // Descriptor bounds and shape validation happen BEFORE reading pixel data.
    for (unsigned mode = 0; mode < 10; ++mode) {
        Fixture f(base, scratch); f.descriptor(4, 4, 2, mode >= 7 ? 6 : 1); f.argb(4, 4, 1);
        if (mode == 0) f.be(f.resource + 28, 3);
        if (mode == 1) f.be(f.resource + 48, 0x400); // 3D
        if (mode == 2) f.be(f.resource + 40, 0x80001414u);
        if (mode == 3) f.be(f.resource + 32, 0x406);
        if (mode == 4) f.be(f.resource + 36, 2048u | (3u << 13)); // width 2049
        if (mode == 5) f.be(f.resource + 36, 3u | (3u << 13) | (1u << 26));
        if (mode == 6) f.be(f.resource + 44, 15u << 6);
        if (mode == 7) f.be(f.resource + 36, 3u | (3u << 13) | (4u << 26));
        if (mode == 8) f.be(f.resource + 36, 3u | (1u << 13) | (5u << 26));
        if (mode == 9) f.be(f.resource + 36, 2u | (2u << 13) | (5u << 26));
        TextureUpload upload(11, 2, 0, mode >= 7 ? 6 : 1);
        expect(!upload.record(f.base, f.resource, f.image, f.data, 0, 0, false), "Invalid resource descriptor accepted");
    }
    for (unsigned mode = 0; mode < 7; ++mode) {
        Fixture f(base, scratch); f.descriptor(4, 4, 2); f.argb(4, 4, 1);
        TextureUpload upload(12, 2, mode == 5 || mode == 6 ? 1 : 0, 1);
        if (mode == 0) f.be(f.image + 16, 2);
        if (mode == 1) f.be(f.image + 20, 2);
        if (mode == 2) f.be(f.image, 0);
        if (mode == 6) f.be(f.image + 16, 2);
        expect(!upload.record(f.base, f.resource, f.image, f.data, mode == 3 ? 2 : 0,
                              mode == 4 ? 1 : 0, mode == 6),
               "Invalid actual or skipped CImage envelope accepted");
        // mode 6 uses the right skipped status but wrong leading dimensions.
    }
    {
        Fixture f(base, scratch); f.descriptor(2, 2, 2); f.skipped(2, 2);
        TextureUpload upload(12, 2, 1, 1);
        expect(upload.record(f.base, f.resource, f.image, 0, 0, 0, true) &&
               !upload.complete(0, 0) && !upload.finish(f.resource),
               "Skipped metadata accepted an actual copy witness");
    }
    // Legacy alpha uses the CImage decoder's explicit coverage format.
    {
        Fixture f(base, scratch); f.descriptor(2, 2, 1);
        f.header(2, 2, 6, 3, 1, 0x40000, 0x814);
        const uint8_t values[]{0, 255, 99, 64, 128, 99};
        std::memcpy(f.base + f.data, values, sizeof(values));
        TextureUpload upload(13, 1, 0, 1);
        expect(upload.record(f.base, f.resource, f.image, f.data, 0, 0, false) && upload.complete(0, 0),
               "decodeUploadImage rejected legacy alpha format 0x40000");
        auto result = upload.finish(f.resource);
        expect(result && result->sourceCodec == 0x40000 && result->pixels.size() == 16,
               "Alpha sourceCodec or byte count lost");
        const uint8_t alpha[]{0, 255, 64, 128};
        for (unsigned i = 0; i < 4; ++i)
            solid(result->pixels, i * 4, 4, {255, 255, 255, alpha[i]});
    }
    // Boundary and overflow checks require no image-sized allocations. Valid
    // <=2048 cube geometry fits the cap; do not bypass that limit just to force
    // a 256 MiB allocation in tests. Exercise the production arithmetic itself.
    static_assert(TextureUpload::byteBudget == 256ull * 1024 * 1024);
    constexpr auto limit = TextureUpload::byteBudget;
    expect(TextureUpload::fitsBudget(0, limit) && TextureUpload::fitsBudget(limit, 0) &&
           TextureUpload::fitsBudget(limit - 1, 1) && !TextureUpload::fitsBudget(limit, 1) &&
           !TextureUpload::fitsBudget(limit + 1, 0) &&
           !TextureUpload::fitsBudget(1, (std::numeric_limits<uint64_t>::max)()) &&
           !TextureUpload::fitsBudget((std::numeric_limits<uint64_t>::max)(), 1),
           "Texture upload fixed byte budget overflowed or accepted an excess");
    // A largest legal logical image with a bad actual extent must reject before
    // the decoder attempts to allocate its claimed output.
    {
        Fixture f(base, scratch); f.descriptor(2048, 2048, 12, 6); f.argb(1, 1, 1);
        TextureUpload upload(14, 12, 0, 6);
        expect(!upload.record(f.base, f.resource, f.image, f.data, 0, 0, false) && upload.ownedBytes() == 0,
               "Large invalid upload allocated pending image data");
    }
}
} // namespace TextureUploadTests

static void testTextureUpload(uint8_t* base = nullptr, uint32_t scratchAddress = 0) {
    TextureUploadTests::run(base, scratchAddress);
    TextureUploadTests::incremental(base, scratchAddress);
}
