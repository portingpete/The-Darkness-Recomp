#pragma once

// Capture owned upload pixels while preserving original resource mip indices.
// Observe before original upload copies; complete only after the original
// lr=822581F0 completion call (caller SP+120=mip, SP+104=face).
// Incremental: record incoming LR=82258F18 (r3 resource, r29 image, r14
// pixels, r5 mip, r4 face); complete after LR=822594E0 (SP+100 mip,+112 face).
#include "renderer/engine/simple_mesh.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace DarkRecomp::Native {

// One initial creation or prefix update, all faces, resource-relative levels.
// Parent must associate a seed with this resource and keep it immutable.
// Thread-confined, non-copyable/non-movable. Nesting belongs to the caller.
// Fresh bytes always come from decodeUploadImage, never from the seed.
// No guest writes, destination reads, or renderer calls.
class TextureUpload {
public:
    static constexpr uint64_t byteBudget = 256ull * 1024 * 1024;
    // Also usable to test the fixed limit without allocating large images.
    static constexpr bool fitsBudget(uint64_t owned, uint64_t additional) noexcept {
        return owned <= byteBudget && additional <= byteBudget - owned;
    }

    // requestedLevels is ALWAYS the full resource level count. Initial mode:
    // seed={}, updateEnd=0. Prefix update: skippedLeading=0,
    // requestedLevels=full resource count, updateEnd=original entry r10.
    // A nonempty seed is mandatory unless updateEnd==requestedLevels (full
    // replacement). The incremental hook must reject entry r10==0: zero here
    // remains the initial-mode sentinel, never an incremental range.
    TextureUpload(uint32_t id, unsigned requestedLevels, unsigned skippedLeading,
                  unsigned faces, std::shared_ptr<const ColorImage> seed = {},
                  unsigned updateEnd = 0) noexcept
        : id_(id), levels_(requestedLevels), first_(skippedLeading), faces_(faces),
          end_(updateEnd ? updateEnd : requestedLevels), seed_(std::move(seed)) {
        // Zero/full-chain and oversized requests must be resolved by the caller
        // from proven original behavior, never guessed or silently clamped here.
        if (!id || !levels_ || levels_ > maxLevels || first_ >= levels_ ||
            (faces_ != 1 && faces_ != 6)) {
            fail("invalid upload request");
            return;
        }
        if ((seed_ && !updateEnd) || (updateEnd && first_) ||
            (!seed_ && updateEnd && updateEnd != levels_) || end_ > levels_ ||
            (seed_ && (first_ || !seed_->valid() || !seed_->authoredMips || seed_->mips.size() + 1 != levels_ ||
                       seed_->firstMip > end_ || seed_->faces != faces_))) {
            fail("invalid prefix update seed or range");
            return;
        }
        if (seed_) {
            seedBytes_ = seed_->bytes();
            if (!fitsBudget(0, seedBytes_)) {
                fail("texture upload seed budget exceeded");
                return;
            }
            // The seed is already RGBA. Its decoder provenance may be a native
            // fetch format rather than a CImage codec, so only fresh slots
            // participate in the upload codec consistency check below.
        }
    }
    TextureUpload(const TextureUpload&) = delete;
    TextureUpload& operator=(const TextureUpload&) = delete;
    TextureUpload(TextureUpload&&) = delete;
    TextureUpload& operator=(TextureUpload&&) = delete;

    uint32_t id() const noexcept { return id_; }
    const char* error() const noexcept { return error_; }
    uint8_t promptOrigin() const noexcept { return promptOrigin_; }
    bool promptConflict() const noexcept { return promptConflict_; }
    bool promptSeen() const noexcept { return promptSeen_; }
    // Sticky origin agreement: every mip/face observation counts, including
    // Unknown (0). First observation arms the latch; any disagreement (known
    // vs different known, or known vs unknown) latches a sticky conflict that
    // later notes cannot clear. A,B,A therefore stays Unknown.
    void notePromptOrigin(uint8_t origin) noexcept {
        if (promptConflict_) { promptOrigin_ = 0; return; }
        if (!promptSeen_) { promptSeen_ = true; promptOrigin_ = origin; return; }
        if (promptOrigin_ != origin) { promptConflict_ = true; promptOrigin_ = 0; }
    }
    // What finish() would publish for the fresh range + seed, without guest
    // access. Conservative: fresh must be seen, conflict-free and known, and
    // any seed tail must carry the same known origin. An unknown fresh prefix
    // never inherits a seed GUI tag, and a conflict never resurrects.
    uint8_t promptResult() const noexcept {
        if (promptConflict_ || !promptSeen_ || !promptOrigin_) return 0;
        if (seed_ && seed_->promptOrigin != promptOrigin_) return 0;
        return promptOrigin_;
    }
    // Includes all seed bytes retained by this collector, even though shared.
    uint64_t ownedBytes() const noexcept { return owned_ + seedBytes_; }

    bool record(uint8_t* base, uint32_t resource, uint32_t image, uint32_t pixels,
                unsigned mip, unsigned face, bool skipped) noexcept {
        if (closed_ || error_) return false;
        try {
            if (!bindResource(base, resource)) return false;
            if (mip >= end_ || face >= faces_ || skipped != (mip < first_))
                return fail("unexpected mip, face, or skipped slot");
            std::array<uint8_t, 48> header{};
            if (!copyRenderMemory(base, image, header.data(), header.size()))
                return fail("unreadable upload CImage");
            const auto w = (std::max)(1u, width_ >> mip);
            const auto h = (std::max)(1u, height_ >> mip);
            if (be(header.data()) != 0x82097610 || be(header.data() + 16) != w ||
                be(header.data() + 20) != h)
                return fail("upload CImage dimensions or type mismatch");
            const uint8_t bit = uint8_t(1u << face);
            auto& level = slots_[mip];
            if (skipped) {
                // In particular, never inspect a borrowed pixels pointer or
                // compressed stream: metadata can name another resident mip.
                if ((level.skipped & bit) && level.headers[face] != header)
                    return fail("conflicting skipped metadata");
                level.headers[face] = header;
                level.skipped |= bit;
                return true;
            }
            const auto flags = be(header.data() + 40);
            if (flags & 0x200) return fail("actual upload is metadata only");
            const uint64_t allocation = be(header.data() + 8);
            const uint64_t storage = be(header.data() + 12);
            if (!allocation || !storage || allocation + storage > 0x100000000ull || !pixels)
                return fail("invalid CImage allocation");
            // decodeUploadImage's BC fallback historically ignored pixels.
            // Validate its stream identity here even with that older decoder.
            if (be(header.data() + 28) == 4 && be(header.data() + 32) == 0x800 &&
                (flags & 0x15000) == 0x5000) {
                std::array<uint8_t, 16> stream{};
                if (storage < stream.size() ||
                    !copyRenderMemory(base, allocation, stream.data(), stream.size()) ||
                    le(stream.data() + 12) != 16 || allocation + 16 != pixels)
                    return fail("upload pixels do not identify owned BC stream");
            }
            const uint64_t bytes = uint64_t(w) * h * 4;
            // Current decoder bounds: RGBA output <=16 MiB and temporary raw
            // input <=8192*2048 bytes. Include both while pending slots live.
            // Keep this bound in step with decodeUploadImage's supported formats.
            constexpr uint64_t decoderScratch = 8192ull * 2048;
            if (!fitsBudget(ownedBytes(), bytes + decoderScratch))
                return fail("texture upload byte budget exceeded");
            ColorImage decoded;
            if (const char* reason = decodeUploadImage(base, image, pixels, decoded))
                return fail(reason);
            if (decoded.width != w || decoded.height != h || decoded.faces != 1 ||
                decoded.firstMip != 0 || decoded.authoredMips || !decoded.mips.empty() ||
                decoded.pixels.size() != bytes)
                return fail("decoder returned incompatible upload image");
            if (haveCodec_ && codec_ != decoded.sourceCodec)
                return fail("upload codec changed");
            // Compare actual bytes, not a digest. Source allocations may be
            // reused/released between calls, so retained data is always owned.
            if (level.seen & bit) {
                if (level.headers[face] != header || level.pixels[face] != decoded.pixels)
                    return fail("conflicting duplicate upload");
            } else {
                level.headers[face] = header;
                level.pixels[face] = std::move(decoded.pixels);
                owned_ += bytes;
                level.seen |= bit;
            }
            codec_ = decoded.sourceCodec;
            haveCodec_ = true;
            // A repeated upload is another copy; an earlier witness cannot
            // prove completion of this invocation.
            level.completed &= uint8_t(~bit);
            return true;
        } catch (...) {
            return fail("texture upload decode or allocation exception");
        }
    }

    bool complete(unsigned mip, unsigned face) noexcept {
        if (closed_ || error_) return false;
        if (!resource_ || mip < first_ || mip >= end_ || face >= faces_ ||
            !(slots_[mip].seen & (1u << face)))
            return fail("copy completion without actual upload");
        slots_[mip].completed |= uint8_t(1u << face);
        return true;
    }

    // Terminal, one-shot operation, even if incomplete. The parent re-resolves
    // the original resource after the upload body and passes that address here.
    // Success transfers ownership; this object retains no published pixel bytes.
    std::shared_ptr<ColorImage> finish(uint32_t resource) noexcept {
        if (closed_ || error_) return {};
        closed_ = true;
        try {
            if (!resource_ || resource != resource_ || !bindResource(base_, resource)) {
                fail("final resource mismatch");
                return {};
            }
            const auto mask = uint8_t((1u << faces_) - 1);
            for (unsigned mip = 0; mip < end_; ++mip) {
                const auto& level = slots_[mip];
                if (mip < first_ ? (level.skipped != mask || level.seen || level.completed)
                                 : (level.seen != mask || level.completed != mask || level.skipped)) {
                    fail("incomplete upload or copy witnesses");
                    return {};
                }
            }
            auto result = std::make_shared<ColorImage>();
            result->width = width_;
            result->height = height_;
            result->faces = faces_;
            result->promptOrigin = promptOrigin_;
            result->sourceCodec = codec_;
            result->authoredMips = true;
            result->firstMip = first_;
            result->mips.resize(levels_ - 1);
            // Account for published, pending, and retained seed bytes together.
            // At most one extra cube level is allocated at a time.
            uint64_t published = 0;
            for (unsigned mip = first_; mip < end_; ++mip) {
                auto& output = mip ? result->mips[mip - 1] : result->pixels;
                auto& level = slots_[mip];
                const size_t faceBytes = level.pixels[0].size();
                const uint64_t levelBytes = uint64_t(faceBytes) * faces_;
                if (faces_ == 1) {
                    output = std::move(level.pixels[0]);
                    owned_ -= levelBytes;
                } else {
                    if (!fitsBudget(ownedBytes() + published, levelBytes)) {
                        fail("texture upload assembly budget exceeded");
                        return {};
                    }
                    output.resize(size_t(levelBytes));
                    for (unsigned face = 0; face < faces_; ++face) {
                        std::memcpy(output.data() + face * faceBytes,
                                    level.pixels[face].data(), faceBytes);
                        std::vector<uint8_t>().swap(level.pixels[face]);
                        owned_ -= faceBytes;
                    }
                }
                published += levelBytes;
            }
            // Conservative seed merge without mutating the queued seed image.
            result->promptOrigin = promptResult();
            promptOrigin_ = result->promptOrigin;
            // Only levels outside the fresh range may come from the seed.
            // Copy their owned vectors so neither success nor failure mutates
            // an older published generation. first_ is necessarily zero here.
            for (unsigned mip = end_; mip < levels_; ++mip) {
                const auto& source = seed_->mips[mip - 1];
                if (!fitsBudget(ownedBytes() + published, source.size())) {
                    fail("texture upload seed merge budget exceeded");
                    return {};
                }
                result->mips[mip - 1] = source;
                published += source.size();
            }
            seed_.reset();
            seedBytes_ = 0;
            return result;
        } catch (...) {
            fail("texture upload assembly exception");
            return {};
        }
    }

private:
    static constexpr unsigned maxLevels = 12; // 2048 through 1, inclusive.
    struct Level {
        std::array<std::vector<uint8_t>, 6> pixels;
        std::array<std::array<uint8_t, 48>, 6> headers{};
        uint8_t seen = 0, completed = 0, skipped = 0;
    };
    static uint32_t be(const uint8_t* p) noexcept {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }
    static uint32_t le(const uint8_t* p) noexcept {
        return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    }
    bool fail(const char* reason) noexcept {
        if (!error_) error_ = reason;
        for (auto& level : slots_)
            for (auto& pixels : level.pixels) std::vector<uint8_t>().swap(pixels);
        owned_ = 0;
        promptOrigin_ = 0;
        promptSeen_ = false;
        promptConflict_ = false;
        seed_.reset();
        seedBytes_ = 0;
        return false;
    }
    bool bindResource(uint8_t* base, uint32_t resource) noexcept {
        std::array<uint8_t, 24> bytes{};
        if (!base || !resource || (resource_ && (resource != resource_ || base != base_)) ||
            !copyRenderMemory(base, uint64_t(resource) + 28, bytes.data(), bytes.size()))
            return fail("unreadable or changed upload resource");
        std::array<uint32_t, 6> fetch{};
        for (unsigned i = 0; i < fetch.size(); ++i) fetch[i] = be(bytes.data() + i * 4);
        if (resource_) {
            if (fetch != fetch_) return fail("upload resource descriptor changed");
            return true;
        }
        const unsigned dimension = (fetch[5] >> 9) & 3;
        const unsigned w = (fetch[2] & 8191) + 1;
        const unsigned h = ((fetch[2] >> 13) & 8191) + 1;
        const unsigned last = (fetch[4] >> 6) & 15;
        if ((fetch[0] & 3) != 2 || (dimension != 1 && dimension != 3) ||
            (fetch[3] & 0x80000000u) || (fetch[1] & 0x400) ||
            w > 2048 || h > 2048 || faces_ != (dimension == 3 ? 6u : 1u) ||
            (dimension == 1 && (fetch[2] >> 26) != 0) ||
            (dimension == 3 && ((fetch[2] >> 26) != 5 || w != h || !std::has_single_bit(w))) ||
            last >= std::bit_width((std::max)(w, h)) || levels_ != last + 1)
            return fail("unsupported or inconsistent resource descriptor");
        if (seed_ && (seed_->width != w || seed_->height != h))
            return fail("prefix update seed dimensions mismatch");
        base_ = base;
        resource_ = resource;
        fetch_ = fetch;
        width_ = w;
        height_ = h;
        return true;
    }

    uint32_t id_ = 0, resource_ = 0, width_ = 0, height_ = 0, codec_ = 0;
    uint8_t promptOrigin_ = 0;
    bool promptSeen_ = false, promptConflict_ = false;
    unsigned levels_ = 0, first_ = 0, faces_ = 0, end_ = 0;
    uint8_t* base_ = nullptr;
    std::array<uint32_t, 6> fetch_{};
    std::array<Level, maxLevels> slots_{};
    std::shared_ptr<const ColorImage> seed_;
    uint64_t owned_ = 0, seedBytes_ = 0;
    const char* error_ = nullptr;
    bool haveCodec_ = false, closed_ = false;
};

} // namespace DarkRecomp::Native
