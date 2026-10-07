#pragma once
#include "runtime/native/menu_pointer.h"
#include <cstring>

namespace MenuPointerGuestFixture {
inline uint32_t root = 0, first = 0, second = 0;
inline unsigned focused = 0, pressed = 0;
inline uint32_t pressedTarget = 0;
inline unsigned choices = 0;
inline uint32_t choiceOwner = 0, choiceWindow = 0, choiceRow = 0;
static PPC_FUNC(focus) {
    ++focused;
    PPC_STORE_U32(first + 84, PPC_LOAD_U32(first + 84) & ~1u);
    PPC_STORE_U32(second + 84, PPC_LOAD_U32(second + 84) & ~1u);
    PPC_STORE_U32(ctx.r3.u32 + 84, PPC_LOAD_U32(ctx.r3.u32 + 84) | 1u);
    ctx.r3.u64 = 1;
}
static PPC_FUNC(press) {
    ++pressed;
    pressedTarget = ctx.r3.u32;
    check(PPC_LOAD_U32(ctx.r4.u32) == 3 && PPC_LOAD_U32(ctx.r4.u32 + 8) == 228 &&
          PPC_LOAD_U32(ctx.r4.u32 + 16) == 160 && PPC_LOAD_U32(ctx.r4.u32 + 24) == 0x437F0000,
          "pointer confirm must use observed original interface/source/value ABI");
    ctx.r3.u64 = 1;
}
static PPC_FUNC(choice) {
    ++choices;
    choiceOwner = ctx.r3.u32;
    choiceWindow = ctx.r4.u32;
    choiceRow = PPC_LOAD_U32(choiceOwner + 4340);
    ctx.r3.u64 = 1;
}
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
}

// Original8244D660 recursion,82452798 button routing and823A8A68 list routing
// execute against a private tree. Only final focus/pressed/choice callbacks are
// deterministic fixtures. Pointer hover must never call Xbox's asserting,
// unused8244DAC8 motion method (the first live regression found that trap).
static void testMenuPointerGuest(PPCContext& ctx) {
    using namespace MenuPointerGuestFixture;
    using namespace DarkRecomp::Native;
    const uint32_t block = memory->allocate(16384);
    check(block != 0, "menu pointer guest fixture allocation failed");
    struct Cleanup { uint32_t block; ~Cleanup() { memory->release(block); } } cleanup{block};
    auto* base = memory->base();
    std::memset(base + block, 0, 16384);
    root = block; first = block + 512; second = block + 1024;
    const uint32_t table = block + 2048, array = block + 2560, items = block + 2624;
    const uint32_t focusAddress = uint32_t(PPC_CODE_BASE) + 4;
    const uint32_t pressAddress = uint32_t(PPC_CODE_BASE) + 8;
    SavedDispatch focusDispatch(base, focusAddress, focus);
    SavedDispatch pressDispatch(base, pressAddress, press);
    std::memcpy(base + table, base + 0x82083730, 312);
    PPC_STORE_U32(table + 60, focusAddress);
    PPC_STORE_U32(table + 264, pressAddress);
    for (const auto window : {root, first, second}) {
        PPC_STORE_U32(window, table);
        PPC_STORE_U32(window + 80, 4);
        PPC_STORE_U32(window + 20, root);
    }
    PPC_STORE_U32(root + 84, 1);
    PPC_STORE_U32(root + 12, array);
    PPC_STORE_U32(array + 4, 2);
    PPC_STORE_U32(array + 24, items);
    PPC_STORE_U32(items, first);
    PPC_STORE_U32(items + 4, second);
    // A modal's keyboard confirm binding is allowed to consume confirm at its
    // root. A pointer must deliver to the hit window, bypassing that binding.
    const uint32_t bindings = block + 2816, bindingItems = block + 2880, binding = block + 2944;
    PPC_STORE_U32(root + 48, bindings);
    PPC_STORE_U32(bindings + 4, 1);
    PPC_STORE_U32(bindings + 24, bindingItems);
    PPC_STORE_U32(bindingItems, binding);
    PPC_STORE_U32(binding + 8, 228);
    PPC_STORE_U32(binding + 12, 0x82065568); // original CStr vtable; null backing is empty
    PPC_STORE_U32(binding + 20, 0x82065568);
    PPC_STORE_U32(root + 136, 640);
    PPC_STORE_U32(root + 140, 480);
    for (const auto window : {first, second}) {
        PPC_STORE_U32(window + 16, root);
        PPC_STORE_U32(window + 128, 64);
        PPC_STORE_U32(window + 132, 64);
        PPC_STORE_U32(window + 136, 256);
        PPC_STORE_U32(window + 140, 128);
    }
    PPC_STORE_U32(first + 84, 8); // a hidden overlapping control must be skipped
    check(menuPointerTreeHasControl(base, root), "visible enabled menu control must establish pointer ownership");
    PPC_STORE_U32(root + 12, 0);
    check(!menuPointerTreeHasControl(base, root), "empty HUD/title window must not establish pointer ownership");
    PPC_STORE_U32(root + 12, array);
    focused = pressed = 0; pressedTarget = 0;
    auto keyboard = ctx;
    keyboard.r1.u64 -= 512;
    const uint32_t keyboardMessage = keyboard.r1.u32 + 112;
    std::memset(base + keyboardMessage, 0, 64);
    PPC_STORE_U32(keyboardMessage, 3);
    PPC_STORE_U32(keyboardMessage + 4, 0x820510B0);
    PPC_STORE_U32(keyboardMessage + 8, 228);
    PPC_STORE_U32(keyboardMessage + 16, 160);
    PPC_STORE_U32(keyboardMessage + 24, 0x437F0000);
    keyboard.r3.u64 = root; keyboard.r4.u64 = keyboardMessage;
    sub_82452650(keyboard, base);
    check(keyboard.r3.u32 == 1 && pressed == 0,
          "fixture root keyboard binding must consume confirm without button activation");
    check(dispatchMenuPointer(ctx, base, root, {100, 100}, false) == second && focused == 1 && pressed == 0,
          "hover must use original hit testing and supported focus without activation");
    check(dispatchMenuPointer(ctx, base, root, {100, 100}, true) == second && pressed == 1 && pressedTarget == second,
          "pointer click must reach hit-window confirm despite root keyboard binding");
    check(dispatchMenuPointer(ctx, base, root, {400, 400}, true) == 0 && focused == 2 && pressed == 1,
          "background click must not activate previously focused control");
    PPC_STORE_U32(second + 80, 0x404);
    check(!menuPointerTreeHasControl(base, root), "hidden and disabled descendants must not claim menu pointer ownership");
    check(dispatchMenuPointer(ctx, base, root, {100, 100}, true) == 0 && pressed == 1,
          "disabled control must not activate");
    PPC_STORE_U32(second + 84, 8);
    check(dispatchMenuPointer(ctx, base, root, {100, 100}, true) == 0 && pressed == 1,
          "hidden controls must not activate");
    PPC_STORE_U32(second + 80, 4);
    PPC_STORE_U32(second + 84, 0);
    PPC_STORE_U32(root + 84, 8);
    check(!menuPointerTreeHasControl(base, root), "hidden owner must not claim menu pointer ownership");

    // Retail chooser Button rows have no SCRIPT_PRESSED. Their menu's real
    // +276 handler invokes +324 with the focused window, while the chooser
    // choice callback reads +4340. Keep the root accelerator from above to
    // prove pointer activation bypasses it for list menus as well.
    const uint32_t chooserTable = block + 6144, rows = block + 6656, rowItems = block + 6720;
    const uint32_t choiceAddress = uint32_t(PPC_CODE_BASE) + 12;
    SavedDispatch choiceDispatch(base, choiceAddress, choice);
    std::memcpy(base + chooserTable, base + 0x820799F0, 340);
    PPC_STORE_U32(chooserTable + 324, choiceAddress);
    PPC_STORE_U32(root, chooserTable);
    PPC_STORE_U32(root + 84, 1);
    PPC_STORE_U32(root + 4320, rows);
    PPC_STORE_U32(rows + 4, 2);
    PPC_STORE_U32(rows + 24, rowItems);
    PPC_STORE_U32(rowItems, first);
    PPC_STORE_U32(rowItems + 4, second);
    PPC_STORE_U32(first + 84, 0);
    PPC_STORE_U32(second + 84, 0);
    PPC_STORE_U32(second + 132, 160);
    PPC_STORE_U32(second + 140, 224);
    choices = 0; choiceOwner = choiceWindow = choiceRow = 0;
    check(dispatchMenuPointer(ctx, base, root, {100, 180}, false) == second &&
          PPC_LOAD_U32(root + 4340) == 1 && choices == 0,
          "chooser hover must synchronize logical selection without running a choice");
    keyboard.r3.u64 = root; keyboard.r4.u64 = keyboardMessage;
    sub_82452650(keyboard, base);
    check(keyboard.r3.u32 == 1 && choices == 0,
          "chooser keyboard accelerator must consume confirm in the fixture");
    check(dispatchMenuPointer(ctx, base, root, {100, 100}, true) == first && choices == 1 &&
          choiceOwner == root && choiceWindow == first && choiceRow == 0 && pressed == 1,
          "immediate chooser click must run the clicked choice through its original menu handler");
    PPC_STORE_U32(root + 4344, 5);
    check(dispatchMenuPointer(ctx, base, root, {100, 180}, true) == second && choices == 2 &&
          choiceWindow == second && choiceRow == 6 && pressed == 1,
          "paginated pointer choice must include the first visible logical row");
    check(dispatchMenuPointer(ctx, base, root, {400, 400}, true) == 0 && choices == 2,
          "chooser background click must not run the focused choice");
    PPC_STORE_U32(second + 80, 0x404);
    check(dispatchMenuPointer(ctx, base, root, {100, 180}, true) == 0 && choices == 2,
          "disabled chooser row must not run a choice");
    PPC_STORE_U32(second + 80, 4);
    PPC_STORE_U32(second + 84, 8);
    check(dispatchMenuPointer(ctx, base, root, {100, 180}, true) == 0 && choices == 2,
          "hidden chooser row must not run a choice");
    PPC_STORE_U32(second + 84, 0);
    // A footer Button outside the owner's list must retain its own callback.
    PPC_STORE_U32(rows + 4, 1);
    check(dispatchMenuPointer(ctx, base, root, {100, 180}, true) == second && pressed == 2 &&
          pressedTarget == second && choices == 2 && PPC_LOAD_U32(root + 4340) == 6,
          "non-list buttons must not activate the chooser's previous row");
    PPC_STORE_U32(rows + 4, 2);
    for (unsigned malformed = 0; malformed < 5; ++malformed) {
        PPC_STORE_U32(root + 4320, rows);
        PPC_STORE_U32(rows + 4, 2);
        PPC_STORE_U32(rows + 24, rowItems);
        PPC_STORE_U32(root + 4344, 5);
        switch (malformed) {
        case 0: PPC_STORE_U32(root + 4320, uint32_t(PPC_MEMORY_SIZE - 4)); break;
        case 1: PPC_STORE_U32(rows + 4, 1025); break;
        case 2: PPC_STORE_U32(rows + 24, uint32_t(PPC_MEMORY_SIZE - 4)); break;
        case 3: PPC_STORE_U32(root + 4344, 0xffffffff); break;
        case 4: PPC_STORE_U32(root + 4344, 0x7fffffff); break;
        }
        check(dispatchMenuPointer(ctx, base, root, {100, 180}, true) == second && choices == 2 &&
              pressed == 3 + malformed && PPC_LOAD_U32(root + 4340) == 6,
              "invalid list metadata must not redirect a button or change logical selection");
    }
    puts("Menu pointer guest hit testing, focus, chooser selection and original confirm ABI passed.");
}
