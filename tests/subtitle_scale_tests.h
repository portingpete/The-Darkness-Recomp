#pragma once

#include "title_text_shadow_tests.h"
#include "title_text_layout_tests.h"
#include "runtime/native/graphics_settings.h"
#include <optional>
#include <string>

// Exercise the retail string conversion, wrapping, centering, line metrics
// and glyph transform. The synthetic font implements only its virtual API.
namespace SubtitleScaleTestDetail {
using namespace DarkRecomp::Native;
using TitleTextShadowTestDetail::getFloat;
using TitleTextShadowTestDetail::putFloat;
using TitleTextLayoutTestDetail::layoutNear;
inline thread_local float fontHeightValue = 14;
inline thread_local std::vector<unsigned> wrapWidths;
struct Draw {
    std::array<float,3> origin{}, horizontal{}, vertical{};
    // Retail B410 passes maximum offsets at SP+92, minimum at SP+84.
    std::array<float,2> clipBegin{}, clipEnd{};
    std::u16string text;
    float fontSize = 0;
    uint32_t color = 0;
};
inline thread_local std::vector<Draw>* draws = nullptr;
inline thread_local uint32_t observedLayoutContext = 0;
inline thread_local std::vector<std::array<float,16>>* heightMatrices = nullptr;
inline thread_local bool observedSubtitleContext = false;
inline unsigned textLength(uint32_t address) {
    auto* base = memory->base();
    unsigned count = 0;
    while (PPC_LOAD_U16(address + count * 2)) ++count;
    return count;
}
static PPC_FUNC(height) {
    observedSubtitleContext |= currentSubtitleRenderContext();
    if (heightMatrices) {
        std::array<float,16> matrix{};
        for (unsigned i = 0; i < matrix.size(); ++i)
            matrix[i] = getFloat(observedLayoutContext + 272 + i * 4);
        heightMatrices->push_back(matrix);
    }
    ctx.f1.f64 = fontHeightValue;
}
static PPC_FUNC(width) { ctx.f1.f64 = double(textLength(ctx.r5.u32)) * fontHeightValue * .5; }
static PPC_FUNC(wrap) {
    wrapWidths.push_back(ctx.r6.u32);
    const unsigned available = unsigned(double(ctx.r6.u32) / (fontHeightValue * .5));
    ctx.r3.u64 = (std::min)(textLength(ctx.r5.u32), available);
}
static PPC_FUNC(draw) {
    check(draws != nullptr, "Subtitle font capture has no output");
    Draw result;
    for (unsigned i = 0; i < 3; ++i) {
        result.origin[i] = getFloat(ctx.r5.u32 + i * 4);
        result.horizontal[i] = getFloat(ctx.r6.u32 + i * 4);
        result.vertical[i] = getFloat(ctx.r7.u32 + i * 4);
    }
    for (unsigned i = 0; i < 2; ++i) {
        result.clipBegin[i] = getFloat(memory->read32(ctx.r1.u32 + 92) + i * 4);
        result.clipEnd[i] = getFloat(memory->read32(ctx.r1.u32 + 84) + i * 4);
    }
    for (unsigned i = 0; i < textLength(ctx.r8.u32); ++i)
        result.text.push_back(char16_t(PPC_LOAD_U16(ctx.r8.u32 + i * 2)));
    result.fontSize = float(ctx.f1.f64);
    result.color = ctx.r10.u32;
    draws->push_back(std::move(result));
    ctx.r3.u64 = 1;
}
struct Fixture {
    TitleTextShadowTestDetail::Fixture font;
    GraphicsSettings settings = graphicsSettings();
    std::array<PPCFunc*,4> oldFunctions{};
    uint32_t lastLineCount = 0;
    std::array<float,16> lastLayoutMatrix{};
    Fixture() {
        const std::array<uint32_t,4> slots{0, 8, 24, 28};
        const std::array<PPCFunc*,4> functions{height, draw, wrap, width};
        for (unsigned i = 0; i < slots.size(); ++i) {
            oldFunctions[i] = PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + slots[i]);
            PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + slots[i]) = functions[i];
        }
        memory->write32(font.block + 1152 + 64, PPC_CODE_BASE + 24);
        memory->write32(font.block + 1152 + 28, PPC_CODE_BASE + 28);
        memory->write32(font.block + 1408 + 4, font.block + 2200);
        memory->write32(font.block + 1408 + 8, font.block + 2600);
    }
    ~Fixture() {
        const std::array<uint32_t,4> slots{0, 8, 24, 28};
        for (unsigned i = 0; i < slots.size(); ++i)
            PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + slots[i]) = oldFunctions[i];
        setGraphicsSettings(settings);
        draws = nullptr;
        heightMatrices = nullptr; observedLayoutContext = 0;
    }
    void rebuild(const PPCContext& initial, NativeVideoMode mode, float depth) {
        check(setNativeVideoMode(mode.width, mode.height), "Cannot select subtitle fixture mode");
        std::memset(memory->base() + font.block, 0, 704);
        putFloat(font.block + 336, float(mode.width) / 640);
        putFloat(font.block + 340, float(mode.height) / 480);
        putFloat(font.block + 632, float(mode.width));
        putFloat(font.block + 636, float(mode.height));
        putFloat(font.block + 656, getFloat(0x82A480C4) + depth);
        memory->write32(font.block + 676, mode.width);
        memory->write32(font.block + 680, mode.height);
        PPCContext guest = initial;
        guest.r3.u64 = font.block; guest.lr = 0x8235F03C;
        currentContext = &guest;
        sub_823471F8(guest, memory->base());
        currentContext = font.oldContext;
        putFloat(font.block + 344, 1); putFloat(font.block + 348, 1);
        const uint32_t rect = font.block + 2048;
        std::memset(memory->base() + rect, 0, 24);
        memory->write32(rect + 16, 640); memory->write32(rect + 20, 480);
    }
    std::vector<Draw> paint(const PPCContext& initial, uint32_t caller,
                            unsigned percent, const std::u16string& text, bool wide,
                            unsigned gap, bool original, bool actor = false,
                            std::optional<float> top = {}, bool nativeEffect = false) {
        auto* base = memory->base();
        auto selected = settings; selected.subtitleScalePercent = percent;
        check(setGraphicsSettings(selected), "Cannot select subtitle scale");
        for (unsigned i = 0; i <= text.size(); ++i) {
            const uint16_t c = i == text.size() ? 0 : uint16_t(text[i]);
            memory->base()[font.block + 2200 + i] = uint8_t(c);
            PPC_STORE_U16(font.block + 2600 + i * 2, c);
        }
        memory->write32(font.block + 1408 + 12, wide ? 0 : 1);
        PPCContext guest = initial;
        guest.fpscr.csr = _mm_getcsr();
        guest.r3.u64 = font.block; guest.r4.u64 = font.block + 2048;
        guest.r5.u64 = font.block + 1024; guest.r6.u64 = font.block + 1408;
        guest.r9.u64 = nativeEffect ? 0x6000 : actor ? 0x2000 : 0x2800;
        guest.r10.u64 = 0x80FFFFFF;
        guest.f1.f64 = actor ? 75 : 60;
        guest.f2.f64 = actor ? (gap == 5 ? 400 : 390) : 376;
        if (top) guest.f2.f64 = *top;
        guest.f3.f64 = 1;
        memory->write32(guest.r4.u32 + 8, actor ? 75 : 0);
        memory->write32(guest.r4.u32 + 16, actor ? 565 : 640);
        guest.lr = caller;
        memory->write32(guest.r1.u32 + 84, 0xFF000000);
        memory->write32(guest.r1.u32 + 92, 0xFF000000);
        memory->write32(guest.r1.u32 + 100, actor ? 490 : 520);
        memory->write32(guest.r1.u32 + 108, 480);
        // Capture both exact caller scopes through the immediate font output;
        // C900 performs the same layout before its queued-shadow branch.
        memory->base()[guest.r1.u32 + 119] = 0;
        memory->write32(guest.r1.u32 + 124, gap);
        const auto before = TitleTextLayoutTestDetail::snapshot(font.block);
        std::array<uint8_t,24> originalRect{};
        std::memcpy(originalRect.data(), base + guest.r4.u32, originalRect.size());
        const bool previousSubtitleContext = currentSubtitleRenderContext();
        std::vector<Draw> output;
        std::vector<std::array<float,16>> matrices;
        heightMatrices = &matrices; observedLayoutContext = font.block;
        draws = &output; wrapWidths.clear(); currentContext = &guest;
        observedSubtitleContext = false;
        if (original) __imp__sub_8234C7E0(guest, memory->base());
        else sub_8234C7E0(guest, memory->base());
        const bool subtitleCaller = caller == 0x8235F24C || caller == 0x8235F28C ||
            caller == 0x823A134C || caller == 0x823A1394 || caller == 0x82331EF8 ||
            caller == 0x82331F38 || caller == 0x8230D96C || caller == 0x8230E32C || caller == 0x8230E36C;
        const bool subtitleProfile = subtitleCaller && (caller != 0x8230D96C || nativeEffect);
        if (observedSubtitleContext != (original ? previousSubtitleContext : subtitleProfile))
            std::fprintf(stderr,"Subtitle ownership caller=%08X percent=%u gap=%u original=%u actor=%u effect=%u y=%g observed=%u\n",
                caller,percent,gap,unsigned(original),unsigned(actor),unsigned(nativeEffect),
                top?double(*top):double(actor?(gap==5?400:390):376),unsigned(observedSubtitleContext));
        check(observedSubtitleContext == (original ? previousSubtitleContext : subtitleProfile),
              "Subtitle rendering ownership was missing or leaked into another text caller");
        check(currentSubtitleRenderContext() == previousSubtitleContext,
              "Subtitle hook did not restore rendering ownership");
        currentContext = font.oldContext; draws = nullptr;
        heightMatrices = nullptr; observedLayoutContext = 0;
        lastLineCount = guest.r3.u32;
        check(!matrices.empty(), "Subtitle fixture captured no native line metrics");
        lastLayoutMatrix = matrices.back();
        check(guest.r1.u32 == initial.r1.u32 && uint32_t(guest.lr) == caller &&
              (nativeEffect ? output.empty() && lastLineCount > 0 : lastLineCount == output.size()),
              "Subtitle hook changed stack, return address or line count");
        check(TitleTextLayoutTestDetail::snapshot(font.block) == before,
              "Subtitle hook leaked borrowed matrix, font scale or draw-context state");
        check(std::memcmp(originalRect.data(), base + font.block + 2048, originalRect.size()) == 0,
              "Subtitle hook leaked borrowed clip bounds or changed the rectangle origin");
        check(memory->read32(initial.r1.u32 + 100) == (actor ? 490 : 520) &&
              memory->read32(initial.r1.u32 + 108) == 480 &&
              memory->read32(initial.r1.u32 + 124) == gap &&
              memory->read32(initial.r1.u32 + 84) == 0xFF000000 &&
              memory->read32(initial.r1.u32 + 92) == 0xFF000000,
              "Subtitle hook changed caller layout/effect arguments");
        return output;
    }
};
inline void sameDraw(const Draw& a, const Draw& b) {
    check(a.origin == b.origin && a.horizontal == b.horizontal && a.vertical == b.vertical &&
          a.clipBegin == b.clipBegin && a.clipEnd == b.clipEnd && a.fontSize == b.fontSize &&
          a.color == b.color && a.text == b.text, "Default or unrelated subtitle draw changed");
}
inline void glyphsInsideClip(const Draw& draw, float height, bool vertical) {
    constexpr double tolerance = .002;
    const double advance = double(draw.text.size()) * height * .5;
    check(draw.clipEnd[0] <= tolerance && draw.clipBegin[0] + tolerance >= advance,
          "Subtitle wrapping retained text beyond the visible horizontal clip");
    if (vertical)
        check(draw.clipEnd[1] <= tolerance && draw.clipBegin[1] + tolerance >= height,
              "Subtitle reflow retained glyphs beyond the visible vertical clip");
}
inline void originalCanvasInsideClip(const Draw& draw, double mx, double my,
                                     double tx, double ty, double left, double right,
                                     double scale) {
    const double minimumX = (draw.origin[0] + double(draw.horizontal[0]) * draw.clipEnd[0] - tx) / mx;
    const double maximumX = (draw.origin[0] + double(draw.horizontal[0]) * draw.clipBegin[0] - tx) / mx;
    const double minimumY = (draw.origin[1] + double(draw.vertical[1]) * draw.clipEnd[1] - ty) / my;
    const double maximumY = (draw.origin[1] + double(draw.vertical[1]) * draw.clipBegin[1] - ty) / my;
    constexpr double tolerance = .002;
    check(minimumX <= left + tolerance && left - minimumX <= scale + tolerance &&
          maximumX + tolerance >= right && maximumX - right <= scale + tolerance &&
          minimumY <= tolerance && -minimumY <= scale + tolerance &&
          maximumY + tolerance >= 480 && maximumY - 480 <= scale + tolerance,
          "Subtitle scaling moved its original physical clipping canvas");
}
struct SyntheticInkBox { double left, top, right, bottom; };
inline SyntheticInkBox syntheticTextCell(const Draw& draw, double mx, double my,
                                        double tx, double ty, double originalSize,
                                        double cellWidth=20, double cellTop=-5, double cellBottom=30) {
    // An opaque synthetic glyph with the measured Text E cell dimensions and
    // bearing. The retail glyph's alpha coverage need not fill its entire cell.
    const double ratio = draw.fontSize / originalSize;
    const double x0 = (draw.origin[0] - tx) / mx;
    const double x1 = (draw.origin[0] + double(draw.horizontal[0]) * cellWidth * ratio - tx) / mx;
    const double y0 = (draw.origin[1] + double(draw.vertical[1]) * cellTop * ratio - ty) / my;
    const double y1 = (draw.origin[1] + double(draw.vertical[1]) * cellBottom * ratio - ty) / my;
    return {(std::min)(x0,x1), (std::min)(y0,y1), (std::max)(x0,x1), (std::max)(y0,y1)};
}
} // namespace SubtitleScaleTestDetail

static void testSubtitleScale(PPCContext& ctx) {
    using namespace SubtitleScaleTestDetail;
    Fixture fixture;
    unsigned cases = 0;
    for (const auto mode : {NativeVideoMode{1280,720}, NativeVideoMode{1920,1200},
                           NativeVideoMode{1720,720}, NativeVideoMode{3440,1440},
                           NativeVideoMode{3840,2160}})
    for (const float heightValue : {14.0f, 24.0f})
    for (const bool wide : {false, true})
    for (const unsigned profile : {0u, 1u, 2u}) {
        fontHeightValue = heightValue;
        fixture.rebuild(ctx, mode, 500);
        const float mx = getFloat(fixture.font.block + 272), my = getFloat(fixture.font.block + 292);
        const float tx = getFloat(fixture.font.block + 320), ty = getFloat(fixture.font.block + 324);
        check(mx != 0 && my != 0, "Subtitle original matrix is degenerate");
        const std::u16string text(150, wide ? u'\u0416' : u'T');
        const bool actor = profile == 2;
        const unsigned gap = profile == 1 ? 0 : heightValue == 14 ? 5 : 10;
        const unsigned width = actor ? 490 : 520;
        const unsigned top = actor ? (gap == 5 ? 400 : 390) : 376;
        const uint32_t foreground = actor ? 0x82331F38 : profile == 1 ? 0x823A1394 : 0x8235F28C;
        const uint32_t shadow = actor ? 0x82331EF8 : profile == 1 ? 0x823A134C : 0x8235F24C;
        const auto retail = fixture.paint(ctx, foreground, 100, text, wide, gap, true, actor);
        const auto neutral = fixture.paint(ctx, foreground, 100, text, wide, gap, false, actor);
        check(retail.size() == neutral.size() && retail.size() > 1, "Subtitle fixture did not wrap");
        for (unsigned i = 0; i < retail.size(); ++i) sameDraw(retail[i], neutral[i]);
        const double bottom = top + retail.size() * heightValue + (retail.size() - 1) * gap;
        for (const unsigned percent : {50u, 70u, 90u, 110u, 130u, 150u, 200u}) {
            const auto enlarged = fixture.paint(ctx, foreground, percent, text, wide, gap, false, actor);
            const double scale = double(percent) / 100;
            std::u16string joined;
            for (const auto& d : enlarged) joined += d.text;
            check(joined == text, "Subtitle reflow dropped or changed localized text");
            check(percent < 100 ? enlarged.size() <= retail.size() : enlarged.size() >= retail.size(),
                  "Subtitle scale changed wrapped line count in the wrong direction");
            check(std::find(wrapWidths.begin(), wrapWidths.end(), width * 100 / percent) != wrapWidths.end(),
                  "Subtitle wrapping did not consume the adjusted original-font width");
            for (unsigned i = 0; i < enlarged.size(); ++i) {
                const auto& d = enlarged[i];
                const double x = (double(d.origin[0]) - tx) / mx;
                const double y = (double(d.origin[1]) - ty) / my;
                if (actor) layoutNear(x, 75, "Actor subtitles lost their left anchor");
                else check(std::abs(x + d.text.size() * heightValue * .5 * scale * .5 - 320) <= scale,
                           "Subtitle reflow lost original horizontal center");
                const double blockHeight = enlarged.size() * heightValue + (enlarged.size() - 1) * gap;
                layoutNear(y, (std::min)(bottom, 470.0) - blockHeight * scale +
                              i * (heightValue + gap) * scale, "Subtitle reflow did not preserve the lower anchor");
                // The longest artificial blocks can exceed the entire canvas.
                // Check vertical clipping whenever the complete glyph fits it.
                glyphsInsideClip(d, heightValue, y >= -.002 && y + heightValue * scale <= 480.002);
                originalCanvasInsideClip(d, mx, my, tx, ty, actor ? 75 : 0,
                                         actor ? 565 : 640, scale);
                for (unsigned axis = 0; axis < 3; ++axis) {
                    layoutNear(d.horizontal[axis], retail[0].horizontal[axis] * (axis < 2 ? scale : 1),
                               "Subtitle horizontal glyph basis has wrong scale");
                    layoutNear(d.vertical[axis], retail[0].vertical[axis] * (axis < 2 ? scale : 1),
                               "Subtitle vertical glyph basis has wrong scale");
                }
                check(d.fontSize == retail[0].fontSize && d.color == retail[0].color &&
                      d.origin[2] == retail[0].origin[2], "Subtitle scale changed font material, fade or depth");
            }
            const auto second = fixture.paint(ctx, shadow, percent, text, wide, gap, false, actor);
            check(second.size() == enlarged.size(), "Subtitle shadow/foreground scopes disagree on reflow");
            for (unsigned i = 0; i < second.size(); ++i) sameDraw(second[i], enlarged[i]);
            const auto repeated = fixture.paint(ctx, foreground, percent, text, wide, gap, false, actor);
            for (unsigned i = 0; i < repeated.size(); ++i) sameDraw(repeated[i], enlarged[i]);
            ++cases;
        }
        for (uint32_t caller : {0x8235F248u, 0x8235F250u, 0x8235F288u, 0x8235F290u,
                                0x823A1348u, 0x823A1350u, 0x823A1390u, 0x823A1398u,
                                0x82331EF4u, 0x82331EFCu, 0x82331F34u, 0x82331F3Cu, 0x8236E070u}) {
            const auto original = fixture.paint(ctx, caller, 200, text, wide, gap, true, actor);
            const auto unaffected = fixture.paint(ctx, caller, 200, text, wide, gap, false, actor);
            check(original.size() == unaffected.size(), "Subtitle scale leaked into another text caller");
            for (unsigned i = 0; i < original.size(); ++i) sameDraw(original[i], unaffected[i]);
        }
    }
    unsigned effectCases = 0, darknessCases = 0;
    for (const auto mode : {NativeVideoMode{1280,720}, NativeVideoMode{3840,2160}})
    for (const bool wide : {false, true})
    for (const unsigned gap : {5u, 10u})
    for (const float top : {gap == 5 ? 405.0f : 395.0f, 342.5f}) {
        fontHeightValue = 14;
        fixture.rebuild(ctx, mode, 500);
        const std::u16string text(150, wide ? u'\u0416' : u'T');
        // The real 0x6000 branch calls C108/BC80 and needs a frame arena to
        // produce glyph records. With no arena, BC80 exits before the font's
        // virtual draw. Capture its native per-line metrics to check the fitted
        // matrix, wrapper result and restoration; effect mesh expansion and
        // rendering are covered by the live-game check, not this fixture.
        fixture.paint(ctx, 0x8230D96C, 100, text, wide, gap, true, true, top, true);
        const auto retailMatrix = fixture.lastLayoutMatrix;
        const unsigned retailLines = fixture.lastLineCount;
        fixture.paint(ctx, 0x8230D96C, 100, text, wide, gap, false, true, top, true);
        check(fixture.lastLineCount == retailLines && fixture.lastLayoutMatrix == retailMatrix,
              "Neutral NPC effect wrapper changed native layout");
        const double bottom = (std::min)(double(top) + retailLines * 14 + (retailLines - 1) * gap, 470.0);
        for (const unsigned percent : {50u, 70u, 90u, 110u, 130u, 150u, 200u}) {
            fixture.paint(ctx, 0x8230D96C, percent, text, wide, gap, false, true, top, true);
            const auto matrix = fixture.lastLayoutMatrix;
            const double scale = double(percent) / 100;
            check((percent < 100 ? fixture.lastLineCount <= retailLines : fixture.lastLineCount >= retailLines) &&
                  std::find(wrapWidths.begin(), wrapWidths.end(), 490 * 100 / percent) != wrapWidths.end(),
                  "NPC effect wrapper did not reflow its final native wrap");
            const double left = (matrix[0] * 75 + matrix[12] - retailMatrix[12]) / retailMatrix[0];
            layoutNear(left, 75, "NPC effect matrix lost its native left anchor");
            const double height = fixture.lastLineCount * 14 + (fixture.lastLineCount - 1) * gap;
            const double lower = (matrix[5] * (top + height) + matrix[13] - retailMatrix[13]) / retailMatrix[5];
            layoutNear(lower, bottom, "NPC effect matrix lost its animated lower anchor");
            layoutNear(matrix[0], retailMatrix[0] * scale, "NPC effect horizontal matrix scale changed");
            layoutNear(matrix[5], retailMatrix[5] * scale, "NPC effect vertical matrix scale changed");
            check(matrix[14] == retailMatrix[14], "NPC effect fitting changed depth");
            ++effectCases;
        }
        for (const uint32_t caller : {0x8230D968u, 0x8230D970u, 0x8236E070u}) {
            fixture.paint(ctx, caller, 200, text, wide, gap, false, true, top, true);
            check(fixture.lastLineCount == retailLines && fixture.lastLayoutMatrix == retailMatrix,
                  "NPC effect fitting leaked into another caller");
        }
        fixture.paint(ctx, 0x8230D96C, 200, text, wide, gap, false, true, top);
        check(fixture.lastLineCount == retailLines && fixture.lastLayoutMatrix == retailMatrix,
              "NPC effect fitting accepted the wrong native flags");
    }
    for (const auto mode : {NativeVideoMode{1280,720}, NativeVideoMode{3840,2160}})
    for (const float heightValue : {14.0f, 24.0f})
    for (const bool wide : {false, true})
    for (const float top : {376.0f, 346.5f, 391.0f, 406.0f}) {
        fontHeightValue = heightValue;
        fixture.rebuild(ctx, mode, 500);
        const std::u16string text(150, wide ? u'\u0416' : u'T');
        const float mx = getFloat(fixture.font.block + 272), my = getFloat(fixture.font.block + 292);
        const float tx = getFloat(fixture.font.block + 320), ty = getFloat(fixture.font.block + 324);
        const auto retail = fixture.paint(ctx, 0x8230E36C, 100, text, wide, 0, true, false, top);
        const auto neutral = fixture.paint(ctx, 0x8230E36C, 100, text, wide, 0, false, false, top);
        check(retail.size() == neutral.size(), "Neutral actor Darkness scope changed line count");
        for (unsigned i = 0; i < retail.size(); ++i) sameDraw(retail[i], neutral[i]);
        const double bottom = (std::min)(double(top) + retail.size() * heightValue, 470.0);
        for (const unsigned percent : {50u, 70u, 90u, 110u, 130u, 150u, 200u}) {
            const auto enlarged = fixture.paint(ctx, 0x8230E36C, percent, text, wide, 0, false, false, top);
            const double scale = double(percent) / 100;
            std::u16string joined;
            for (unsigned i = 0; i < enlarged.size(); ++i) {
                const auto& d = enlarged[i];
                joined += d.text;
                const double x = (double(d.origin[0]) - tx) / mx;
                const double y = (double(d.origin[1]) - ty) / my;
                check(std::abs(x + d.text.size() * heightValue * .5 * scale * .5 - 320) <= scale,
                      "Actor Darkness scope lost its animated center");
                layoutNear(y, bottom - enlarged.size() * heightValue * scale + i * heightValue * scale,
                           "Actor Darkness scope lost its animated lower anchor");
                glyphsInsideClip(d, heightValue, y >= -.002 && y + heightValue * scale <= 480.002);
                originalCanvasInsideClip(d, mx, my, tx, ty, 0, 640, scale);
                for (unsigned axis = 0; axis < 3; ++axis) {
                    layoutNear(d.horizontal[axis], retail[0].horizontal[axis] * (axis < 2 ? scale : 1),
                               "Actor Darkness horizontal basis scale changed");
                    layoutNear(d.vertical[axis], retail[0].vertical[axis] * (axis < 2 ? scale : 1),
                               "Actor Darkness vertical basis scale changed");
                }
                check(d.fontSize == retail[0].fontSize && d.color == retail[0].color &&
                      d.origin[2] == retail[0].origin[2], "Actor Darkness fitting changed font, fade or depth");
            }
            check(joined == text &&
                  (percent < 100 ? enlarged.size() <= retail.size() : enlarged.size() >= retail.size()) &&
                  std::find(wrapWidths.begin(), wrapWidths.end(), 520 * 100 / percent) != wrapWidths.end(),
                  "Actor Darkness scope changed text or did not reflow");
            const auto second = fixture.paint(ctx, 0x8230E32C, percent, text, wide, 0, false, false, top);
            check(second.size() == enlarged.size(), "Actor Darkness paired scopes disagree on reflow");
            for (unsigned i = 0; i < second.size(); ++i) sameDraw(second[i], enlarged[i]);
            ++darknessCases;
        }
        for (const uint32_t caller : {0x8230E328u, 0x8230E330u, 0x8230E368u, 0x8230E370u, 0x8236E070u}) {
            const auto unaffected = fixture.paint(ctx, caller, 200, text, wide, 0, false, false, top);
            check(unaffected.size() == retail.size(), "Actor Darkness fitting leaked into another caller");
            for (unsigned i = 0; i < unaffected.size(); ++i) sameDraw(retail[i], unaffected[i]);
        }
    }
    // Enzo's long sentence exposed a difference between complete wrapped text
    // and visible glyphs: at 90%, the former490-unit clip shrank to441 units.
    // Cover an explicit native '|' break as well as a literal unbroken sentence.
    unsigned clippingCases = 0;
    fontHeightValue = 14;
    fixture.rebuild(ctx, {3440,1440}, 500);
    const double clippingMx = getFloat(fixture.font.block + 272);
    const double clippingMy = getFloat(fixture.font.block + 292);
    const double clippingTx = getFloat(fixture.font.block + 320);
    const double clippingTy = getFloat(fixture.font.block + 324);
    for (const std::u16string& text : {
            std::u16string(u"Uh-uh... I steer clear of that little rat like he had the bubonic plague. What the hell did you do?"),
            std::u16string(u"Uh-uh... I steer clear of that little rat like he had the bubonic plague. What the|hell did you do?")})
    for (const bool wide : {false, true}) {
        std::u16string visibleText;
        for (char16_t character : text) if (character != u'|') visibleText += character;
        const auto retail = fixture.paint(ctx, 0x82331F38, 100, text, wide, 5, true, true);
        const auto neutral = fixture.paint(ctx, 0x82331F38, 100, text, wide, 5, false, true);
        check(retail.size() == neutral.size(), "Default Enzo subtitle changed native line count");
        for (unsigned i = 0; i < retail.size(); ++i) sameDraw(retail[i], neutral[i]);
        for (unsigned percent : {50u, 90u, 100u, 150u, 200u}) {
            const auto foreground = fixture.paint(ctx, 0x82331F38, percent, text, wide, 5, false, true);
            const auto shadow = fixture.paint(ctx, 0x82331EF8, percent, text, wide, 5, false, true);
            check(foreground.size() == shadow.size() && !foreground.empty(),
                  "Enzo subtitle foreground/shadow lost a wrapped line");
            std::u16string joined;
            for (unsigned i = 0; i < foreground.size(); ++i) {
                const auto& draw = foreground[i];
                sameDraw(draw, shadow[i]);
                glyphsInsideClip(draw, 14, true);
                originalCanvasInsideClip(draw, clippingMx, clippingMy, clippingTx, clippingTy,
                                         75, 565, double(percent) / 100);
                joined += draw.text;
            }
            check(joined == visibleText, "Enzo subtitle reflow dropped literal text or an authored line");
            ++clippingCases;
        }
    }
    // The actor canvas scales glyphs independently of line origins. Exercise
    // actual native C900/B410 line output with a synthetic Text-like ink cell,
    // rather than equating the integer line metric with the visible glyph size.
    unsigned spacingCases = 0;
    constexpr uint32_t nativeFontSizeAddress = 0x82A47C90;
    constexpr unsigned spacingGap = 10;
    // Actual stock/Russian ORIGINALSIZE values and E-cell ink metrics differ.
    for (const float textOriginalSize : {26.9230785f, 22.3076935f})
    for (const NativeVideoMode mode : {NativeVideoMode{1280,720}, NativeVideoMode{3440,1440}})
    for (float contextualScale : {1.3f, 1.5f})
    for (float globalFontSize : {14.0f, -1.0f})
    for (const std::u16string& text : {std::u16string(u"E"), std::u16string(u"E|E"),
            std::u16string(u"Oh, oh. Ap\u00e1rtate de esa peque\u00f1a rata como de la peste bub\u00f3nica.|\u00bfQu\u00e9 diablos has hecho?"),
            // Representative test phrases exercise each supported script/locale, not captured dialogue.
            std::u16string(u"Keep away from that little rat as if he carried the plague.|What have you done?"),
            std::u16string(u"Hüte dich vor dieser kleinen Ratte wie vor der Pest.|Was hast du bloß getan?"),
            std::u16string(u"Éloigne-toi de ce petit rat comme de la peste.|Qu'est-ce que tu as fait ?"),
            std::u16string(u"Stai lontano da quel piccolo ratto come dalla peste.|Che cosa hai fatto?"),
            std::u16string(u"Держись подальше от этой маленькой крысы, словно от чумы.|Что же ты наделал? Ёжик, ёжик!")}) {
        const float previousHeight = fontHeightValue;
        fixture.rebuild(ctx, mode, 500);
        struct RestoreFontMetrics {
            uint32_t context;
            uint32_t global = memory->read32(nativeFontSizeAddress);
            uint32_t horizontal, vertical;
            float height;
            ~RestoreFontMetrics() {
                memory->write32(nativeFontSizeAddress, global);
                memory->write32(context + 344, horizontal);
                memory->write32(context + 348, vertical);
                fontHeightValue = height;
            }
        } restore{fixture.font.block, memory->read32(nativeFontSizeAddress),
                  memory->read32(fixture.font.block + 344), memory->read32(fixture.font.block + 348),
                  previousHeight};
        fontHeightValue = textOriginalSize;
        putFloat(nativeFontSizeAddress, globalFontSize);
        putFloat(fixture.font.block + 344, contextualScale);
        putFloat(fixture.font.block + 348, contextualScale);
        const double mx = getFloat(fixture.font.block + 272), my = getFloat(fixture.font.block + 292);
        const double tx = getFloat(fixture.font.block + 320), ty = getFloat(fixture.font.block + 324);
        const auto retail = fixture.paint(ctx, 0x82331F38, 100, text, true, spacingGap, true, true);
        const unsigned authoredLines = text.find(u'|') == std::u16string::npos ? 1 : 2;
        check(retail.size() >= authoredLines, "Synthetic Text ink fixture lost an authored line");
        const double nativeHeight = std::trunc(textOriginalSize);
        const double adjustedAdvance = std::ceil((nativeHeight + spacingGap) * double(contextualScale));
        const double resolvedSize = globalFontSize < 0 ? textOriginalSize : globalFontSize;
        std::u16string literalText;
        for (char16_t character : text) if (character != u'|') literalText += character;
        for (unsigned percent : {50u, 90u, 100u, 150u, 200u}) {
            const auto foreground = fixture.paint(ctx, 0x82331F38, percent, text, true, spacingGap, false, true);
            const auto shadow = fixture.paint(ctx, 0x82331EF8, percent, text, true, spacingGap, false, true);
            check(foreground.size() >= authoredLines && shadow.size() == foreground.size(),
                  "Speech fitting lost an authored line or shadow");
            const double scale = foreground[0].horizontal[0] / retail[0].horizontal[0];
            check(scale > 0 && scale <= double(percent) / (100 * contextualScale) + .00001,
                  "Speech retained the extra actor-font magnification");
            std::u16string joined;
            std::optional<SyntheticInkBox> previousInk;
            for (unsigned i = 0; i < foreground.size(); ++i) {
                const auto& draw = foreground[i];
                sameDraw(draw, shadow[i]);
                joined += draw.text;
                layoutNear(draw.fontSize, resolvedSize, "Contextual subtitle leading changed native font size");
                for (unsigned axis = 0; axis < 3; ++axis) {
                    layoutNear(draw.horizontal[axis], retail[0].horizontal[axis] * (axis < 2 ? scale : 1),
                               "Contextual subtitle leading changed its horizontal glyph basis");
                    layoutNear(draw.vertical[axis], retail[0].vertical[axis] * (axis < 2 ? scale : 1),
                               "Contextual subtitle leading changed its vertical glyph basis");
                }
                const double baseline = (draw.origin[1] - ty) / my;
                if (i)
                    layoutNear(baseline - (foreground[i-1].origin[1] - ty) / my,
                               adjustedAdvance * scale, "Contextual subtitle leading ignored glyph scale or authored gap");
                const bool russianMetrics = textOriginalSize < 25;
                const auto ink = syntheticTextCell(draw, mx, my, tx, ty, textOriginalSize,
                    russianMetrics ? 13 : 20, russianMetrics ? -4 : -5, russianMetrics ? 25 : 30);
                check(ink.top > 400 && ink.bottom < 480,
                      "Speech ink overlaps the character-name band or bottom edge");
                layoutNear((draw.origin[0] - tx) / mx, 75, "Speech lost its left margin");
                const double right = 75 + draw.text.size() * resolvedSize * .5 * contextualScale * scale;
                check(right <= 565.01, "Speech glyphs exceed their horizontal canvas");
                if (previousInk)
                    check(previousInk->bottom <= ink.top + .002,
                          "Adjacent synthetic Text glyph ink rectangles overlap");
                previousInk = ink;
                if (ink.left >= 75 && ink.right <= 565 && ink.top >= 0 && ink.bottom <= 480) {
                    const double ratio = draw.fontSize / textOriginalSize;
                    check(draw.clipEnd[0] <= .002 && draw.clipBegin[0] + .002 >= 20 * ratio &&
                          draw.clipEnd[1] <= -5 * ratio + .002 && draw.clipBegin[1] + .002 >= 30 * ratio,
                          "A complete synthetic Text cell was clipped inside the original canvas");
                }
                // Clip offsets are in authored line coordinates; the glyph's
                // independent context scale must not be applied a second time.
                auto canvasDraw = draw;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    canvasDraw.horizontal[axis] /= contextualScale;
                    canvasDraw.vertical[axis] /= contextualScale;
                }
                originalCanvasInsideClip(canvasDraw, mx, my, tx, ty, 75, 565, scale);
                check(draw.color == retail[0].color && draw.origin[2] == retail[0].origin[2],
                      "Contextual subtitle leading changed native fade, material or depth");
            }
            check(joined == literalText, "Speech fitting dropped translated text");
            check((foreground[0].origin[1] - ty) / my >= 410 - .002,
                  "Speech fitting grew into the character-name area");
            layoutNear((foreground.back().origin[1] - ty) / my,
                       466 - std::ceil(nativeHeight * contextualScale) * scale,
                       "Speech fitting lost its lower anchor");
            const auto unrelated = fixture.paint(ctx, 0x82331F3C, percent, text, true, spacingGap, false, true);
            check(unrelated.size() == retail.size(), "Contextual subtitle leading leaked into another caller");
            for (unsigned i = 0; i < retail.size(); ++i) sameDraw(retail[i], unrelated[i]);
            ++spacingCases;
        }
        if (text == u"E|E") {
            fixture.paint(ctx, 0x8230D96C, 100, text, true, spacingGap, false, true, 390.f, true);
            const auto resting = fixture.lastLayoutMatrix;
            const double restingY = (resting[5] * 390 + resting[13] - ty) / my;
            check(restingY >= 410 - .002, "Animated speech rests in the character-name area");
            fixture.paint(ctx, 0x8230D96C, 100, text, true, spacingGap, false, true, 342.5f, true);
            const auto moving = fixture.lastLayoutMatrix;
            const double movingY = (moving[5] * 342.5 + moving[13] - ty) / my;
            layoutNear(movingY - restingY, -47.5, "Speech fitting lost the native effect motion");
            layoutNear(moving[0], resting[0], "Animated speech changed size during its motion");
        }
        if (text == u"E") {
            putFloat(fixture.font.block + 344, 1e-30f);
            const auto original = fixture.paint(ctx, 0x82331F38, 200, text, true, spacingGap, true, true);
            const auto unsupported = fixture.paint(ctx, 0x82331F38, 200, text, true, spacingGap, false, true);
            check(original.size() == unsupported.size(), "Unsupported speech scale lost a line");
            for (unsigned i = 0; i < original.size(); ++i) sameDraw(original[i], unsupported[i]);
        }
    }
    struct RestoreMathMode {
        unsigned value = _mm_getcsr();
        ~RestoreMathMode() { _mm_setcsr(value); }
    } restoreMathMode;
    fontHeightValue = 14;
    fixture.rebuild(ctx, {3840,2160}, 500);
    for (unsigned rounding : {_MM_ROUND_NEAREST, _MM_ROUND_UP, _MM_ROUND_DOWN, _MM_ROUND_TOWARD_ZERO}) {
        _mm_setcsr((restoreMathMode.value & ~_MM_ROUND_MASK) | rounding);
        const auto output = fixture.paint(ctx, 0x8235F28C, 130, std::u16string(150, u'\u0416'), true, 5, false);
        check(!output.empty() && std::find(wrapWidths.begin(), wrapWidths.end(), 400u) != wrapWidths.end(),
              "Subtitle wrap width depends on guest rounding mode");
        check((_mm_getcsr() & _MM_ROUND_MASK) == rounding,
              "Subtitle scaling changed guest rounding mode");
    }
    std::printf("NativeSubtitleScaleContract: %u synthetic-font draw cases plus %u no-frame NPC effect layout cases, %u ultrawide Enzo clipping cases and %u contextual Text ink/leading cases; ASCII/Unicode, original-size output, authored-line reflow, visible glyph bounds, preserved clipping canvas, nonoverlapping synthetic ink, animated lower/center/left anchors, caller scopes, depth/fade and borrowed-state restoration. NPC effect mesh expansion requires live-game validation.\n", cases + darknessCases, effectCases, clippingCases, spacingCases);
}
