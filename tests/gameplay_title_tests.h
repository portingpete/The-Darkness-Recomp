#pragma once

#include "title_text_layout_tests.h"
#include "title_text_shadow_tests.h"

static void testGameplayTitles(PPCContext& ctx) {
    using namespace TitleTextShadowTestDetail;
    TitleTextShadowTestDetail::Fixture fixture;
    // Focused-object names and transient character/location titles both
    // right-align measured text to x=790 on an 853x480 canvas.
    struct Title { uint32_t caller; float y; };
    constexpr std::array<Title,2> titles{{{0x8216C954,372}, {0x8216CB44,392}}};
    constexpr float textWidth = 80, x = 790 - textWidth;
    unsigned cases = 0;
    for (const auto mode : {NativeVideoMode{1152,720}, NativeVideoMode{1680,1050},
                           NativeVideoMode{960,720}, NativeVideoMode{1280,720},
                           NativeVideoMode{1920,1080}, NativeVideoMode{1720,720}}) {
        check(setNativeVideoMode(mode.width, mode.height), "Cannot select gameplay title mode");
        TitleTextLayoutTestDetail::rebuild(ctx, fixture.block, mode);
        putFloat(fixture.block + 344, 1);
        putFloat(fixture.block + 348, 1);
        const auto originalMatrix = TitleTextLayoutTestDetail::matrix(fixture.block);
        const double tx = getFloat(fixture.block + 320), ty = getFloat(fixture.block + 324);
        const double logicalScale = getFloat(fixture.block + 336);
        const double worldPerPixelX = getFloat(fixture.block + 272) / logicalScale;
        const double worldPerPixelY = getFloat(fixture.block + 292) / logicalScale;
        const bool narrow = uint64_t(mode.width) * 9 < uint64_t(mode.height) * 16;
        for (const auto title : titles)
        for (const bool wide : {false, true})
        for (const uint32_t color : {0x80FFFFFFu, 0xFFFFFFFFu}) {
            const auto before = paint(ctx, fixture, title.caller, color, 1, false, wide, true, 0, x, title.y);
            const auto after = paint(ctx, fixture, title.caller, color, 1, false, wide, false, 0, x, title.y);
            check(before.size() == 1 && after.size() == 1,
                  "Gameplay title changed the original number of font passes");
            const auto& a = after.front();
            const auto& b = before.front();
            auto rightEdge = [&](const Draw& draw) {
                return (double(draw.origin[0]) + textWidth * draw.horizontal[0] - tx) / worldPerPixelX;
            };
            if (narrow) {
                check(rightEdge(b) > mode.width, "Original gameplay name did not reproduce clipping");
                check(rightEdge(a) <= mode.width && rightEdge(a) > mode.width * .8,
                      "Gameplay character name still crosses the right screen edge");
                const double top = (double(a.origin[1]) - ty) / worldPerPixelY;
                const double bottom = top + 16 * double(a.vertical[1]) / worldPerPixelY;
                check(top >= 0 && bottom <= mode.height, "Gameplay title left the vertical viewport");
                TitleTextLayoutTestDetail::layoutNear(
                    double(a.horizontal[0]) / b.horizontal[0], double(a.vertical[1]) / b.vertical[1],
                    "Gameplay title fitting stretched glyphs");
            } else {
                sameDraw(a, b);
            }
            check(a.text == b.text && a.fontSize == b.fontSize && a.scale == b.scale &&
                  a.color == color && a.origin[2] == b.origin[2] &&
                  a.clipBegin == b.clipBegin && a.clipEnd == b.clipEnd,
                  "Gameplay title fitting changed glyphs, fade, depth or authored clipping");
            check(TitleTextLayoutTestDetail::matrix(fixture.block) == originalMatrix &&
                  getFloat(fixture.block + 336) == logicalScale &&
                  getFloat(fixture.block + 340) == logicalScale &&
                  getFloat(fixture.block + 344) == 1 && getFloat(fixture.block + 348) == 1,
                  "Gameplay title fitting leaked borrowed matrix or scale state");
            // A subsequent unrelated string must not inherit the title scope.
            const auto unrelated = paint(ctx, fixture, 0x12345678, color, 1, false, wide, false, 0, x, title.y);
            check(unrelated.size() == before.size(), "Gameplay title scope changed the next draw");
            sameDraw(unrelated.front(), b);
            ++cases;
        }
    }
    std::printf("Gameplay title contracts passed (%u cases)\n", cases);
}
