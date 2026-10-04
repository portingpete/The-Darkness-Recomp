#pragma once
#include "runtime/native/keyboard_menu.h"
#include <cstring>

namespace MenuKeyboardStatusFixture {
inline uint32_t backing = 0;
inline unsigned allocations = 0;
struct SavedDispatch {
    uint8_t* base;
    uint32_t address;
    PPCFunc* previous;
    SavedDispatch(uint8_t* b, uint32_t a, PPCFunc* replacement)
        : base(b), address(a), previous(PPC_LOOKUP_FUNC(b, a)) {
        PPC_LOOKUP_FUNC(base, address) = replacement;
    }
    ~SavedDispatch() { PPC_LOOKUP_FUNC(base, address) = previous; }
};
// Original CStr construction, assignment, and release still execute. Keep a
// fixture reference because this test does not initialize the engine allocator.
static PPC_FUNC(allocateString) {
    check(ctx.r5.u32 < 254 && allocations < 64,
          "keyboard status fixture string allocation overflow");
    const auto data = backing + allocations++ * 256;
    PPC_STORE_U16(data, 0x4002);
    std::memcpy(base + data + 2, base + ctx.r4.u32, ctx.r5.u32);
    PPC_STORE_U8(data + 2 + ctx.r5.u32, 0);
    PPC_STORE_U32(ctx.r3.u32 + 4, data);
}
}

static void testMenuKeyboardStatus(PPCContext& ctx) {
    using namespace MenuKeyboardStatusFixture;
    using namespace DarkRecomp::Native;
    const uint32_t block = memory->allocate(0x8000);
    check(block != 0, "keyboard status fixture allocation failed");
    auto* base = memory->base();
    std::memset(base + block, 0, 0x8000);
    const auto window = block, region = block + 512, property = block + 600;
    const auto value = block + 632, raw = block + 768;
    backing = block + 8192; allocations = 0;
    struct Cleanup {
        uint32_t block;
        ~Cleanup() { endKeyboardMenu(); cancelKeyboardMenuCapture(); memory->release(block); }
    } cleanup{block};
    SavedDispatch stringAllocation(base, PPC_LOAD_U32(0x82065568 + 104), allocateString);
    auto call = ctx; call.r3.u64 = window;
    sub_82450A20(call, base); // Exact original constructor used by CubeText's factory.
    // Reproduce the factory's derived vtable/CStr initialization without its
    // engine allocator; all base window layout and focus defaults are original.
    PPC_STORE_U32(window, 0x82074100);
    PPC_STORE_U32(window + 280, 0x82065568);
    check(!(PPC_LOAD_U32(window + 80) & 4),
          "original status CubeText constructor unexpectedly accepts focus");
    for (const auto target : {window + 96, region}) {
        PPC_STORE_U32(target, 0); PPC_STORE_U32(target + 4, 456);
        PPC_STORE_U32(target + 8, 640); PPC_STORE_U32(target + 12, 480);
    }
    const auto string = [&](uint32_t object, const char* text) {
        std::strcpy(reinterpret_cast<char*>(base + raw), text);
        call = ctx; call.r3.u64 = object; call.r4.u64 = raw;
        sub_821F86A8(call, base);
    };
    const auto label = [&] {
        return std::string(reinterpret_cast<const char*>(base + PPC_LOAD_U32(window + 284) + 2));
    };
    string(window + 168, "DARKRECOMP_KEYBOARD_STATUS");
    string(property, "TEXT"); string(value, "sc, READY");
    call = ctx; call.r3.u64 = window; call.r4.u64 = region;
    call.r5.u64 = property; call.r6.u64 = value;
    sub_8239A3D0(call, base); // Initial short TEXT cannot resize this CubeText.
    check(label() == "sc, READY" && PPC_LOAD_U32(region + 8) == 640 &&
          PPC_LOAD_U32(window + 104) == 640 && !(PPC_LOAD_U32(window + 80) & 4),
          "short initial status label shrank the row or enabled focus");

    // Keep the original unparented early-out: the full glyph renderer requires
    // the live CubeMenu. This executes the real paint wrapper and original paint
    // function while isolating label ownership/ABI from engine startup.
    endKeyboardMenu(); cancelKeyboardMenuCapture();
    check(beginKeyboardMenu(defaultKeyboardBindings()), "keyboard status model did not start");
    const auto paint = [&] {
        call = ctx; call.r3.u64 = window;
        sub_8239A4E8(call, base);
        check(call.r1.u64 == ctx.r1.u64 && call.r31.u64 == ctx.r31.u64 &&
              PPC_LOAD_U32(window + 96) == 0 && PPC_LOAD_U32(window + 100) == 456 &&
              PPC_LOAD_U32(window + 104) == 640 && PPC_LOAD_U32(window + 108) == 480 &&
              !(PPC_LOAD_U32(window + 80) & 4),
              "status repaint changed original layout, focus, or saved PPC registers");
        check(label().starts_with("sc, ") && label().size() - 4 <= 40,
              "keyboard status text exceeds the authored forty-glyph row");
    };
    paint();
    check(label() == keyboardMenuLabel("darkrecomp.keyboard.status"),
          "native status ID did not refresh through original CubeText paint");
    check(keyboardMenuAction("darkrecomp.keyboard.bind.8.0"), "status capture did not begin");
    paint();
    check(label().find("PRESS A KEY") != std::string::npos,
          "wider capture status did not replace the initial status text");
    keyboardMenuKeyEvent('J', true, false, false);
    keyboardMenuKeyEvent('J', false, false, false);
    paint();
    check(label().find("BINDING UPDATED") != std::string::npos,
          "shorter updated status did not replace the capture message");

    string(window + 168, "UNRELATED_CUBE_TEXT");
    string(window + 280, "sc, RETAIL LABEL");
    paint();
    check(label() == "sc, RETAIL LABEL",
          "keyboard status wrapper changed an unrelated original CubeText");
}
