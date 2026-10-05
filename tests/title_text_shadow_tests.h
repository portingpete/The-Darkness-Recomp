#pragma once

// Run the original string conversion, line painter and font transform. Only
// the font's virtual output is captured, so the oracle observes the original
// guest routine's draw positions/colors instead of duplicating shadow math.
#include <array>
#include <bit>
#include <cmath>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_8234C680);
extern "C" PPC_FUNC(__imp__sub_8234C7E0);

namespace TitleTextShadowTestDetail {
using namespace DarkRecomp::Native;

struct Draw {
    std::array<float,3> origin{}, horizontal{}, vertical{};
    std::array<float,2> scale{}, clipBegin{}, clipEnd{};
    std::array<uint16_t,6> text{};
    float fontSize = 0;
    uint32_t color = 0;
};
inline thread_local std::vector<Draw>* output = nullptr;

inline float getFloat(uint32_t address) {
    return std::bit_cast<float>(memory->read32(address));
}
inline void putFloat(uint32_t address, float value) {
    memory->write32(address, std::bit_cast<uint32_t>(value));
}
static PPC_FUNC(fontHeight) { ctx.f1.f64 = 16; }
static PPC_FUNC(lineHeight) { /* Preserve the original caller's scaled height. */ }
static PPC_FUNC(stringAscii) { ctx.r3.u64 = memory->read32(ctx.r3.u32 + 4); }
static PPC_FUNC(stringWide) { ctx.r3.u64 = memory->read32(ctx.r3.u32 + 8); }
static PPC_FUNC(stringKind) { ctx.r3.u64 = memory->read32(ctx.r3.u32 + 12); }
static PPC_FUNC(fontDraw) {
    check(output != nullptr, "Title font capture has no output");
    Draw draw;
    for (unsigned i = 0; i < 3; ++i) {
        draw.origin[i] = getFloat(ctx.r5.u32 + i * 4);
        draw.horizontal[i] = getFloat(ctx.r6.u32 + i * 4);
        draw.vertical[i] = getFloat(ctx.r7.u32 + i * 4);
    }
    for (unsigned i = 0; i < 2; ++i) {
        draw.scale[i] = getFloat(ctx.r9.u32 + i * 4);
        draw.clipBegin[i] = getFloat(memory->read32(ctx.r1.u32 + 92) + i * 4);
        draw.clipEnd[i] = getFloat(memory->read32(ctx.r1.u32 + 84) + i * 4);
    }
    for (unsigned i = 0; i < draw.text.size(); ++i) {
        const uint32_t at = ctx.r8.u32 + i * 2;
        draw.text[i] = uint16_t((uint16_t(base[at]) << 8) | base[at + 1]);
    }
    draw.fontSize = float(ctx.f1.f64);
    draw.color = ctx.r10.u32;
    output->push_back(draw);
    ctx.r3.u64 = 1;
}

struct Fixture {
    uint32_t block = memory->allocate(4096);
    NativeVideoMode oldMode = nativeVideoMode();
    PPCContext* oldContext = currentContext;
    std::vector<Draw>* oldOutput = output;
    std::array<PPCFunc*,6> oldFunctions{};
    bool installed = false;
    Fixture() {
        check(block != 0, "Cannot allocate title shadow fixture");
        auto* base = memory->base();
        std::memset(base + block, 0, 4096);
        const std::array<PPCFunc*,6> probes{fontHeight, lineHeight, fontDraw,
            stringAscii, stringWide, stringKind};
        for (unsigned i = 0; i < probes.size(); ++i) {
            oldFunctions[i] = PPC_LOOKUP_FUNC(base, PPC_CODE_BASE + i * 4);
            PPC_LOOKUP_FUNC(base, PPC_CODE_BASE + i * 4) = probes[i];
        }
        installed = true;
        memory->write32(block + 1024, block + 1152); // Font and its vtable.
        memory->write32(block + 1152 + 20, PPC_CODE_BASE);
        memory->write32(block + 1152 + 52, PPC_CODE_BASE + 4);
        memory->write32(block + 1152 + 100, PPC_CODE_BASE + 8);
        memory->write32(block + 1408, block + 1536); // CStr and its vtable.
        memory->write32(block + 1408 + 4, block + 1792);
        memory->write32(block + 1408 + 8, block + 1824);
        memory->write32(block + 1536 + 36, PPC_CODE_BASE + 12);
        memory->write32(block + 1536 + 56, PPC_CODE_BASE + 16);
        memory->write32(block + 1536 + 76, PPC_CODE_BASE + 20);
        std::memcpy(base + block + 1792, "TITLE", 6);
        for (unsigned i = 0; i < 6; ++i) {
            base[block + 1824 + i * 2] = 0;
            base[block + 1825 + i * 2] = uint8_t("TITLE"[i]);
        }
        // Original CRect layout: offset, clip minimum, clip maximum.
        memory->write32(block + 2048 + 16, 853);
        memory->write32(block + 2048 + 20, 480);
    }
    ~Fixture() {
        if (installed)
            for (unsigned i = 0; i < oldFunctions.size(); ++i)
                PPC_LOOKUP_FUNC(memory->base(), PPC_CODE_BASE + i * 4) = oldFunctions[i];
        currentContext = oldContext;
        output = oldOutput;
        setNativeVideoMode(oldMode.width, oldMode.height);
        if (block) memory->release(block);
    }
    void matrix(NativeVideoMode mode) {
        check(setNativeVideoMode(mode.width, mode.height), "Title shadow video mode rejected");
        std::memset(memory->base() + block, 0, 704);
        // Immediate font path: no frame allocator, the real B410 transform
        // still runs before it calls the font's virtual output method.
        putFloat(block + 272, 2.0f / float(mode.width));
        putFloat(block + 292, -2.0f / float(mode.height));
        putFloat(block + 312, 1);
        putFloat(block + 320, -.75f);
        putFloat(block + 324, .8f);
        putFloat(block + 328, .25f);
        putFloat(block + 332, 1);
        putFloat(block + 336, float(mode.width) / 853);
        putFloat(block + 340, float(mode.height) / 480);
        putFloat(block + 344, 1);
        putFloat(block + 348, 1);
        memory->write32(block + 676, mode.width);
        memory->write32(block + 680, mode.height);
    }
};

inline bool sameFloat(float a, float b) { return std::abs(a - b) < .000001f; }
inline void sameDraw(const Draw& a, const Draw& b) {
    for (unsigned i = 0; i < 3; ++i)
        check(sameFloat(a.origin[i], b.origin[i]) && sameFloat(a.horizontal[i], b.horizontal[i]) &&
              sameFloat(a.vertical[i], b.vertical[i]), "Title foreground position or font transform changed");
    for (unsigned i = 0; i < 2; ++i)
        check(sameFloat(a.scale[i], b.scale[i]) && sameFloat(a.clipBegin[i], b.clipBegin[i]) &&
              sameFloat(a.clipEnd[i], b.clipEnd[i]), "Title foreground scale or clip changed");
    check(a.text == b.text && sameFloat(a.fontSize, b.fontSize) && a.color == b.color,
          "Title foreground glyphs, size, color or fade changed");
}

inline std::vector<Draw> paint(const PPCContext& initial, Fixture& fixture, uint32_t caller,
                              uint32_t color, float fontScale, bool cString, bool wide,
                              bool original, uint32_t flags = 0) {
    PPCContext guest;
    std::memcpy(&guest, &initial, sizeof guest);
    guest.r3.u64 = fixture.block;
    guest.r4.u64 = fixture.block + 2048;
    guest.r5.u64 = fixture.block + 1024;
    guest.r6.u64 = fixture.block + (cString ? 1792 : 1408);
    guest.r9.u64 = flags;
    guest.r10.u64 = color;
    guest.f1.f64 = 250.25;
    guest.f2.f64 = 100.5;
    guest.f3.f64 = fontScale;
    guest.lr = caller;
    memory->write32(fixture.block + 1408 + 12, wide ? 0 : 1);
    memory->write32(guest.r1.u32 + 84, 0x11223344);
    memory->write32(guest.r1.u32 + 92, 0xAABBCCDD);
    memory->write32(guest.r1.u32 + 100, 853);
    memory->write32(guest.r1.u32 + 108, 480);
    memory->base()[guest.r1.u32 + 119] = 0;
    memory->write32(guest.r1.u32 + 124, 0);
    std::vector<Draw> draws;
    output = &draws;
    currentContext = &guest;
    if (cString) {
        if (original) __imp__sub_8234C680(guest, memory->base());
        else sub_8234C680(guest, memory->base());
    } else {
        if (original) __imp__sub_8234C7E0(guest, memory->base());
        else sub_8234C7E0(guest, memory->base());
    }
    output = fixture.oldOutput;
    currentContext = fixture.oldContext;
    check(guest.r1.u32 == initial.r1.u32 && uint32_t(guest.lr) == caller && guest.r3.u32 == 1,
          "Title shadow changed the original guest stack, return address or result");
    check(memory->read32(initial.r1.u32 + 84) == 0x11223344 &&
          memory->read32(initial.r1.u32 + 92) == 0xAABBCCDD,
          "Title shadow did not restore the caller's effect colors");
    return draws;
}
} // namespace TitleTextShadowTestDetail

static void testTitleTextShadow(PPCContext& ctx) {
    using namespace TitleTextShadowTestDetail;
    Fixture fixture;
    unsigned cases = 0;
    constexpr std::array<uint32_t,6> objectCallers{
        0x8233F508, 0x8233F5A4, 0x8233F5F4, 0x8233F6FC, 0x8233F790, 0x8233F980};
    for (const auto mode : {NativeVideoMode{1280,720}, NativeVideoMode{1920,1200}, NativeVideoMode{1024,768}}) {
        fixture.matrix(mode);
        for (const uint32_t alpha : {0x20u, 0x80u, 0xFFu}) {
            const uint32_t color = (alpha << 24) | 0xFFFFFF;
            for (const float fontScale : {.75f, 1.0f}) {
                for (unsigned path = 0; path < 3; ++path) {
                    const bool cString = path == 0, wide = path == 2;
                    for (unsigned i = 0; i < (cString ? 1u : unsigned(objectCallers.size())); ++i) {
                        const uint32_t caller = cString ? 0x8233F8E4 : objectCallers[i];
                        const auto before = paint(ctx, fixture, caller, color, fontScale, cString, wide, true);
                        const auto after = paint(ctx, fixture, caller, color, fontScale, cString, wide, false);
                        check(before.size() == 1 && after.size() == 2, "Title shadow did not add exactly one original font draw");
                        sameDraw(before[0], after[1]);
                        check(after[0].color == (color & 0xFF000000) && after[0].text == before[0].text &&
                              sameFloat(after[0].fontSize, before[0].fontSize),
                              "Title shadow changed glyphs/font size or lost the foreground fade");
                        for (unsigned axis = 0; axis < 3; ++axis)
                            check(sameFloat(after[0].origin[axis], before[0].origin[axis] +
                                  before[0].horizontal[axis] + before[0].vertical[axis]),
                                  "Title shadow is not one original canvas unit below/right of the unchanged foreground");
                        ++cases;
                    }
                }
            }
        }
        for (const bool cString : {false, true}) {
            const uint32_t titleCaller = cString ? 0x8233F8E4 : objectCallers[0];
            for (const uint32_t caller : {titleCaller - 4, titleCaller + 4, 0x8236E070u, 0x8236E0C4u}) {
                const auto before = paint(ctx, fixture, caller, 0x80FFFFFF, 1, cString, false, true);
                const auto after = paint(ctx, fixture, caller, 0x80FFFFFF, 1, cString, false, false);
                check(before.size() == 1 && after.size() == 1, "Title shadow changed an adjacent or unrelated text caller");
                sameDraw(before[0], after[0]);
            }
            const auto before = paint(ctx, fixture, titleCaller, 0x80FFFFFF, 1, cString, false, true, 0x400);
            const auto after = paint(ctx, fixture, titleCaller, 0x80FFFFFF, 1, cString, false, false, 0x400);
            check(before.size() == 2 && after.size() == 2, "Title shadow changed an already decorated original draw");
            for (unsigned i = 0; i < before.size(); ++i) sameDraw(before[i], after[i]);
        }
    }
    std::printf("Title text shadow: %u original guest font checks; unchanged foreground/clip, alpha-matched shadow, ASCII/Unicode, caller scoping and original effects.\n", cases);
}
