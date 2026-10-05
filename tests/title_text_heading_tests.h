#pragma once

#include "title_text_layout_tests.h"
#include "title_text_shadow_tests.h"

// The coordinate-tagged intro heading calls C7E0 directly with wrapping and
// a selected, transparent retail shadow. Exercise its actual guest painter.
namespace TitleTextHeadingTestDetail {
using namespace TitleTextShadowTestDetail;
constexpr uint32_t headingCaller = 0x822A196C;
constexpr float authoredX = 110, authoredY = 320;
inline thread_local uint32_t expectedWrapWidth = 0;

static PPC_FUNC(wrapText) {
    check(ctx.r6.u32 == expectedWrapWidth, "Heading hook changed the original native wrapping width");
    unsigned length = 0;
    while (base[ctx.r5.u32 + length * 2] || base[ctx.r5.u32 + length * 2 + 1]) ++length;
    ctx.r3.u64 = length;
}

struct Fixture {
    TitleTextShadowTestDetail::Fixture font;
    PPCFunc* originalWrap = PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + 24);
    Fixture() {
        PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + 24) = wrapText;
        memory->write32(font.block + 1152 + 64, PPC_CODE_BASE + 24);
    }
    ~Fixture() { PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + 24) = originalWrap; }

    void rebuild(const PPCContext& initial, NativeVideoMode mode, float depth) {
        check(setNativeVideoMode(mode.width, mode.height), "Cannot select heading fixture mode");
        // Retail titles use a preprojection world plane, not a clip-space
        // matrix. Supply a clean CView cache before the original AOT rebuild;
        // context+688 is its dirty byte, so metadata canaries go in afterward.
        std::memset(memory->base() + font.block, 0, TitleTextLayoutTestDetail::contextBytes);
        putFloat(font.block + 336, float(mode.height) / 480);
        putFloat(font.block + 340, float(mode.height) / 480);
        putFloat(font.block + 632, float(mode.width));
        putFloat(font.block + 636, float(mode.height));
        putFloat(font.block + 656, getFloat(0x82A480C4) + depth);
        memory->write32(font.block + 676, mode.width);
        memory->write32(font.block + 680, mode.height);
        PPCContext guest;
        std::memcpy(&guest, &initial, sizeof guest);
        guest.r3.u64 = font.block;
        guest.lr = headingCaller;
        currentContext = &guest;
        __imp__sub_823471F8(guest, memory->base());
        currentContext = font.oldContext;
        check(guest.r1.u32 == initial.r1.u32 && uint32_t(guest.lr) == headingCaller,
              "Original heading matrix rebuild changed stack or return address");
        for (const auto word : TitleTextLayoutTestDetail::matrix(font.block))
            check(std::isfinite(std::bit_cast<float>(word)), "Original heading fixture matrix is not finite");
        memory->write32(font.block + 344, 0x3F19999A);
        memory->write32(font.block + 348, 0x3F4CCCCD);
        memory->write32(font.block + 688, 0xA17E59C3);
        // The live intro borrows the complete native-pixel clipping rectangle.
        const uint32_t rect = font.block + 2048;
        std::memset(memory->base() + rect, 0, 24);
        memory->write32(rect + 16, mode.width);
        memory->write32(rect + 20, mode.height);
        memory->write32(font.block + 1408 + 12, 1); // ASCII CStr.
    }

    std::vector<Draw> paint(const PPCContext& initial, uint32_t caller, uint32_t color, bool original) {
        PPCContext guest;
        std::memcpy(&guest, &initial, sizeof guest);
        guest.r3.u64 = font.block;
        guest.r4.u64 = font.block + 2048;
        guest.r5.u64 = font.block + 1024;
        guest.r6.u64 = font.block + 1408;
        guest.r9.u64 = 0x2200;
        guest.r10.u64 = color;
        guest.f1.f64 = authoredX;
        guest.f2.f64 = authoredY;
        guest.f3.f64 = 1;
        guest.lr = caller;
        memory->write32(guest.r1.u32 + 84, color & 0xFF000000);
        memory->write32(guest.r1.u32 + 92, 0);
        const uint32_t wrapWidth = memory->read32(guest.r4.u32 + 16) - uint32_t(authoredX);
        const uint32_t wrapHeight = memory->read32(guest.r4.u32 + 20) - uint32_t(authoredY);
        expectedWrapWidth = wrapWidth;
        memory->write32(guest.r1.u32 + 100, wrapWidth);
        memory->write32(guest.r1.u32 + 108, wrapHeight);
        memory->base()[guest.r1.u32 + 119] = 0;
        memory->write32(guest.r1.u32 + 124, 0);
        const auto matrixBefore = TitleTextLayoutTestDetail::matrix(font.block);
        std::array<uint32_t,4> scalesBefore{};
        std::array<uint8_t,24> rectBefore{};
        for (unsigned i = 0; i < scalesBefore.size(); ++i)
            scalesBefore[i] = memory->read32(font.block + 336 + i * 4);
        std::memcpy(rectBefore.data(), memory->base() + guest.r4.u32, rectBefore.size());
        std::vector<Draw> draws;
        output = &draws;
        currentContext = &guest;
        if (original) __imp__sub_8234C7E0(guest, memory->base());
        else sub_8234C7E0(guest, memory->base());
        output = font.oldOutput;
        currentContext = font.oldContext;
        check(guest.r1.u32 == initial.r1.u32 && uint32_t(guest.lr) == caller && guest.r3.u32 == 1,
              "Heading hook changed the original guest stack, return address or result");
        check(memory->read32(initial.r1.u32 + 84) == (color & 0xFF000000) &&
              memory->read32(initial.r1.u32 + 92) == 0 &&
              memory->read32(initial.r1.u32 + 100) == wrapWidth &&
              memory->read32(initial.r1.u32 + 108) == wrapHeight,
              "Heading hook did not restore the caller's effect colors or layout arguments");
        check(TitleTextLayoutTestDetail::matrix(font.block) == matrixBefore,
              "Heading hook leaked its fitted matrix into the next draw");
        for (unsigned i = 0; i < scalesBefore.size(); ++i)
            check(memory->read32(font.block + 336 + i * 4) == scalesBefore[i],
                  "Heading hook changed logical canvas or font scales");
        check(std::memcmp(rectBefore.data(), memory->base() + font.block + 2048, rectBefore.size()) == 0,
              "Heading hook changed the caller's clipping rectangle");
        return draws;
    }
};
} // namespace TitleTextHeadingTestDetail

static void testTitleTextHeading(PPCContext& ctx) {
    using namespace TitleTextHeadingTestDetail;
    TitleTextHeadingTestDetail::Fixture fixture;
    unsigned cases = 0;
    for (const auto mode : {NativeVideoMode{960,720}, NativeVideoMode{1152,720}, NativeVideoMode{1280,720}})
    for (const float depth : {64.0f, 500.0f}) {
        fixture.rebuild(ctx, mode, depth);
        const bool narrow = uint64_t(mode.width) * 9 < uint64_t(mode.height) * 16;
        const double fit = narrow ? double(mode.width) / (853 * (double(mode.height) / 480)) : 1;
        const double pixelScale = double(mode.height) / 480 * fit;
        const double margin = narrow ? (mode.height - 480 * pixelScale) * .5 : 0;
        const double tx = getFloat(fixture.font.block + 320), ty = getFloat(fixture.font.block + 324);
        const double worldPerPixelX = getFloat(fixture.font.block + 272) / getFloat(fixture.font.block + 336);
        const double worldPerPixelY = getFloat(fixture.font.block + 292) / getFloat(fixture.font.block + 340);
        check(std::isfinite(tx) && std::isfinite(ty) && std::isfinite(worldPerPixelX) &&
              std::isfinite(worldPerPixelY) && worldPerPixelX != 0 && worldPerPixelY != 0 &&
              std::abs(tx) > 1 && std::abs(ty) > 1,
              "Heading fixture did not rebuild a retail world plane beyond clip space");
        for (const uint32_t color : {0x80FFFFFFu, 0xFFFFFFFFu}) {
            const auto before = fixture.paint(ctx, headingCaller, color, true);
            const auto after = fixture.paint(ctx, headingCaller, color, false);
            check(before.size() == 2 && after.size() == 2,
                  "Intro heading did not preserve its two original font passes");
            check(before[0].color == 0 && after[0].color == (color & 0xFF000000) &&
                  after[1].color == color, "Intro heading did not make its existing shadow visible with matching alpha");
            for (unsigned pass = 0; pass < 2; ++pass) {
                const auto& a = after[pass];
                const auto& b = before[pass];
                // Convert the captured world-space output using the untouched
                // original plane. This pixel oracle retains the retail +1
                // foreground and +2 shadow offsets and catches bad tx fitting.
                const double offset = pass == 0 ? 2 : 1;
                const double actualX = (double(a.origin[0]) - tx) / worldPerPixelX;
                const double actualY = (double(a.origin[1]) - ty) / worldPerPixelY;
                const double expectedX = (authoredX + offset) * pixelScale;
                const double expectedY = margin + (authoredY + offset) * pixelScale;
                if (!std::isfinite(actualX) || !std::isfinite(actualY) ||
                    std::abs(actualX - expectedX) >= .002 || std::abs(actualY - expectedY) >= .002)
                    std::fprintf(stderr, "Heading %ux%u depth=%g pass=%u color=%08X actualPx=(%.9g,%.9g) expectedPx=(%.9g,%.9g) originalPx=(%.9g,%.9g) worldBefore=(%.9g,%.9g) worldAfter=(%.9g,%.9g) tx/ty=(%.9g,%.9g) worldPerPx=(%.9g,%.9g) basisBefore=(%.9g,%.9g) basisAfter=(%.9g,%.9g) sx/sy=(%.9g,%.9g) viewport=(%u,%u,%u,%u)\n",
                        mode.width, mode.height, double(depth), pass, color, actualX, actualY, expectedX, expectedY,
                        (double(b.origin[0]) - tx) / worldPerPixelX, (double(b.origin[1]) - ty) / worldPerPixelY,
                        double(b.origin[0]), double(b.origin[1]), double(a.origin[0]), double(a.origin[1]), tx, ty,
                        worldPerPixelX, worldPerPixelY, double(b.horizontal[0]), double(b.vertical[1]),
                        double(a.horizontal[0]), double(a.vertical[1]), getFloat(fixture.font.block + 336),
                        getFloat(fixture.font.block + 340), memory->read32(fixture.font.block + 668),
                        memory->read32(fixture.font.block + 672), memory->read32(fixture.font.block + 676),
                        memory->read32(fixture.font.block + 680));
                TitleTextLayoutTestDetail::layoutNear(actualX, expectedX, "Intro heading horizontal pixel position did not fit its authored canvas");
                TitleTextLayoutTestDetail::layoutNear(actualY, expectedY, "Intro heading vertical pixel position did not retain symmetric margins");
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const double axisFit = axis < 2 ? fit : 1;
                    TitleTextLayoutTestDetail::layoutNear(a.horizontal[axis], b.horizontal[axis] * axisFit,
                                                          "Intro heading horizontal glyph basis changed incorrectly");
                    TitleTextLayoutTestDetail::layoutNear(a.vertical[axis], b.vertical[axis] * axisFit,
                                                          "Intro heading vertical glyph basis changed incorrectly");
                }
                check(a.text == b.text && a.fontSize == b.fontSize && a.scale == b.scale && a.origin[2] == b.origin[2],
                      "Intro heading fitting changed glyphs, font size or depth");
                for (unsigned axis = 0; axis < 2; ++axis)
                    check(sameFloat(a.clipBegin[axis], b.clipBegin[axis]) && sameFloat(a.clipEnd[axis], b.clipEnd[axis]),
                          "Intro heading fitting changed the original native clip bounds");
            }
            if (!narrow) sameDraw(before[1], after[1]);
            // The very next text draw borrows the same draw context. Both
            // adjacent and unrelated callers must retain their original output.
            for (const uint32_t caller : {headingCaller - 4, headingCaller + 4, 0x8236E070u}) {
                const auto original = fixture.paint(ctx, caller, color, true);
                const auto untouched = fixture.paint(ctx, caller, color, false);
                check(original.size() == untouched.size(), "Heading scope leaked into another text caller");
                for (unsigned i = 0; i < original.size(); ++i) sameDraw(original[i], untouched[i]);
            }
            ++cases;
        }
    }
    std::printf("TitleTextHeadingContract: %u original guest heading checks; retail-depth pixel fitting, native clips, visible existing shadow, 16:9 and restored caller/context scope.\n", cases);
}
