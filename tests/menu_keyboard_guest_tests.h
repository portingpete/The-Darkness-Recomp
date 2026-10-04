#pragma once
#include "runtime/native/keyboard_menu.h"
#include "runtime/native/keyboard_menu_guest.h"
#include <cstring>

namespace MenuKeyboardGuestFixture {
inline uint32_t owner = 0, button = 0, backing = 0;
inline uint64_t callerStack = 0;
inline unsigned constructed = 0, dispatched = 0;

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

// The original CStr constructor/destructor still execute. Only their backing
// allocation method is replaced, because this fixture does not start the game
// and initialize its engine allocator.
static PPC_FUNC(retainScript) {
    constexpr char script[] = "cg_submenu('darkrecomp_keyboard_1')";
    check(ctx.r5.u32 == sizeof(script) - 1 &&
          std::memcmp(base + ctx.r4.u32, script, sizeof(script)) == 0,
          "keyboard entry did not construct the exact original submenu script");
    ++constructed;
    PPC_STORE_U32(backing, 0x40020000); // Retained byte backing: destructor leaves one reference.
    std::memcpy(base + backing + 2, script, sizeof(script));
    PPC_STORE_U32(ctx.r3.u32 + 4, backing);
}

static PPC_FUNC(dispatchScript) {
    constexpr char script[] = "cg_submenu('darkrecomp_keyboard_1')";
    check(ctx.r3.u32 == button && ctx.r1.u64 == callerStack - 512 &&
          ctx.r4.u32 == uint32_t(callerStack - 512 + 384),
          "keyboard entry script dispatch ignored its private PPC context");
    check(PPC_LOAD_U32(ctx.r4.u32 + 4) == backing &&
          std::memcmp(base + backing + 2, script, sizeof(script)) == 0,
          "keyboard entry did not pass a retained original CStr to window+308");
    ++dispatched;
    // cg_submenu defers page creation. The current name/root remain Controls
    // until the original FrontEnd dispatcher runs on the next update.
    PPC_STORE_U32(owner + 308, 2);
    std::strcpy(reinterpret_cast<char*>(base + owner + 56), "darkrecomp_keyboard_1");
    ctx.r3.u64 = 1;
    ctx.r4.u64 = 0xbad0cafe; // Callback volatile state must not leak into caller.
}
}

static void testMenuKeyboardGuest(PPCContext& ctx) {
    using namespace MenuKeyboardGuestFixture;
    using namespace DarkRecomp::Native;
    const uint32_t block = memory->allocate(0x6000);
    check(block != 0, "keyboard guest fixture allocation failed");
    auto* base = memory->base();
    std::memset(base + block, 0, 0x6000);
    owner = block;
    const uint32_t foreign = block + 0x1000, root = block + 0x2000;
    button = block + 0x2400;
    const uint32_t buttonTable = block + 0x2800;
    backing = block + 0x3000;

    const auto page = [&](uint32_t frontend, const char* name, int depth = 2) {
        PPC_STORE_U32(frontend, 0x82071A10);
        const auto title = std::string_view(name);
        const uint32_t pageOffset = title.starts_with("darkrecomp_keyboard_") &&
            title.back() >= '1' && title.back() <= '4' ? uint32_t(title.back() - '1') * 128 : 0;
        const uint32_t currentRoot = root + (frontend == foreign ? 512 : pageOffset);
        PPC_STORE_U32(currentRoot, 0x82078708);
        PPC_STORE_U32(currentRoot + 84, 0x81);
        PPC_STORE_U8(currentRoot + 88, 0);
        PPC_STORE_U32(frontend + 16, currentRoot);
        PPC_STORE_U32(frontend + 464, uint32_t(depth));
        PPC_STORE_U32(frontend + 308, 0xffffffff);
        const uint32_t str = frontend + 384 + uint32_t(depth) * 8;
        const uint32_t raw = frontend + 0x800;
        PPC_STORE_U32(str, 0x82065568);
        PPC_STORE_U32(str + 4, raw);
        PPC_STORE_U16(raw, 0x4002);
        std::strcpy(reinterpret_cast<char*>(base + raw + 2), name);
        PPC_STORE_U32(frontend + 344 + uint32_t(depth) * 4, PPC_LOAD_U32(frontend + 16));
    };
    struct Cleanup {
        uint32_t block, owner, foreign;
        PPCContext ctx;
        uint8_t* base;
        ~Cleanup() {
            for (const auto frontend : {owner, foreign}) {
                PPC_STORE_U32(frontend + 464, 0xffffffff);
                updateKeyboardMenuGuest(ctx, base, frontend);
            }
            endKeyboardMenu();
            cancelKeyboardMenuCapture();
            memory->release(block);
        }
    } cleanup{block, owner, foreign, ctx, base};

    page(owner, "options_controller", 1);
    page(foreign, "options_video", 1);
    endKeyboardMenu();
    cancelKeyboardMenuCapture();
    std::memcpy(base + buttonTable, base + 0x82074850, 312);
    const uint32_t scriptAddress = uint32_t(PPC_CODE_BASE) + 16;
    PPC_STORE_U32(buttonTable + 308, scriptAddress);
    PPC_STORE_U32(button, buttonTable);
    SavedDispatch scriptDispatch(base, scriptAddress, dispatchScript);
    const auto constructAddress = PPC_LOAD_U32(0x82065568 + 104);
    SavedDispatch backingDispatch(base, constructAddress, retainScript);
    callerStack = ctx.r1.u64;
    constructed = dispatched = 0;
    auto call = ctx;
    check(activateKeyboardMenu(call, base, button, "darkrecomp.keybindings") &&
          constructed == 1 && dispatched == 1,
          "keyboard entry failed to use original CStr and script callback ABI");
    check(call.r1.u64 == ctx.r1.u64 && call.r3.u64 == ctx.r3.u64 &&
          call.r4.u64 == ctx.r4.u64 && call.r31.u64 == ctx.r31.u64,
          "keyboard entry changed original caller registers");
    updateKeyboardMenuGuest(call, base, owner);
    check(keyboardMenuAction("darkrecomp.keyboard.bind.8.0") && keyboardMenuCaptureActive(),
          "deferred entry was discarded while Controls still owned the current root");
    cancelKeyboardMenuCapture();

    page(owner, "darkrecomp_keyboard_1");
    updateKeyboardMenuGuest(call, base, owner);
    check(keyboardMenuAction("darkrecomp.keyboard.bind.8.0") && keyboardMenuCaptureActive(),
          "owned keyboard page did not permit capture");
    page(foreign, "options_controller", 1);
    updateKeyboardMenuGuest(call, base, foreign);
    check(keyboardMenuCaptureActive(), "unrelated FrontEnd cancelled the owned keyboard session");
    check(keyboardMenuKeyEvent('J', true) && !keyboardMenuCaptureActive(),
          "owned keyboard capture did not accept a new physical key");
    keyboardMenuKeyEvent('J', false);
    const auto stagedJump = keyboardMenuLabel("darkrecomp.keyboard.bind.8.0");
    for (const char* name : {"darkrecomp_keyboard_2", "darkrecomp_keyboard_3", "darkrecomp_keyboard_4"}) {
        page(owner, name);
        updateKeyboardMenuGuest(call, base, owner);
        check(PPC_LOAD_U32(owner + 464) == 2 &&
              keyboardMenuLabel("darkrecomp.keyboard.bind.8.0") == stagedJump,
              "same-depth keyboard page switch lost staged bindings");
    }

    check(keyboardMenuAction("darkrecomp.keyboard.cancel"), "keyboard Cancel action was not recognized");
    page(foreign, "darkrecomp_keyboard_2");
    updateKeyboardMenuGuest(call, base, foreign);
    check(PPC_LOAD_U32(foreign + 308) == 0xffffffff,
          "another FrontEnd consumed the owned keyboard close request");
    const uint32_t currentRoot = PPC_LOAD_U32(owner + 16);
    updateKeyboardMenuGuest(call, base, owner);
    check(PPC_LOAD_U32(owner + 308) == 4 && PPC_LOAD_U32(owner + 312) == 1 &&
          PPC_LOAD_U32(owner + 464) == 2 && PPC_LOAD_U32(owner + 16) == currentRoot,
          "keyboard close failed to defer original cg_prevmenu without destroying the current root");
    check(call.r1.u64 == ctx.r1.u64 && call.r3.u64 == ctx.r3.u64 && call.r31.u64 == ctx.r31.u64,
          "keyboard deferred close corrupted caller context");
    PPC_STORE_U32(owner + 308, 0xffffffff);
    updateKeyboardMenuGuest(call, base, owner);
    check(PPC_LOAD_U32(owner + 308) == 0xffffffff, "consumed close request popped a second time");
    page(owner, "options_controller", 1);
    updateKeyboardMenuGuest(call, base, owner);
    keyboardMenuAction("darkrecomp.keyboard.bind.8.0");
    check(!keyboardMenuCaptureActive(), "Back to Controls retained keyboard capture ownership");

    beginKeyboardMenu(defaultKeyboardBindings());
    page(owner, "darkrecomp_keyboard_2");
    updateKeyboardMenuGuest(call, base, owner);
    keyboardMenuAction("darkrecomp.keyboard.save");
    KeyboardMenuSaveRequest failed, retry;
    check(takeKeyboardMenuSaveRequest(failed), "Save failure fixture did not create a host request");
    reportKeyboardMenuSave(failed.id, false);
    updateKeyboardMenuGuest(call, base, owner);
    check(PPC_LOAD_U32(owner + 308) == 0xffffffff && !keyboardMenuInputBlocked(),
          "failed host Save popped the menu or left navigation blocked");
    keyboardMenuAction("darkrecomp.keyboard.save");
    check(takeKeyboardMenuSaveRequest(retry) && retry.id != failed.id,
          "failed Save could not be retried with a fresh request");
    reportKeyboardMenuSave(retry.id, true);
    updateKeyboardMenuGuest(call, base, owner);
    check(PPC_LOAD_U32(owner + 308) == 4 && PPC_LOAD_U32(owner + 312) == 1,
          "successful host Save did not use the original deferred Back operation");
    page(owner, "options_controller", 1);
    updateKeyboardMenuGuest(call, base, owner);

    beginKeyboardMenu(defaultKeyboardBindings());
    page(owner, "darkrecomp_keyboard_1");
    updateKeyboardMenuGuest(call, base, owner);
    keyboardMenuAction("darkrecomp.keyboard.save");
    KeyboardMenuSaveRequest request;
    check(takeKeyboardMenuSaveRequest(request), "owned keyboard Save did not create a host request");
    page(owner, "options_controller", 1);
    updateKeyboardMenuGuest(call, base, owner);
    reportKeyboardMenuSave(request.id, true);
    page(owner, "options_video", 1);
    updateKeyboardMenuGuest(call, base, owner);
    check(PPC_LOAD_U32(owner + 308) == 0xffffffff && !takeKeyboardMenuCloseRequest(),
          "stale host Save completion closed a later unrelated menu");
    beginKeyboardMenu(defaultKeyboardBindings());
    page(owner, "darkrecomp_keyboard_3");
    updateKeyboardMenuGuest(call, base, owner);
    reportKeyboardMenuSave(request.id, true);
    check(!takeKeyboardMenuCloseRequest(), "stale Save completion closed a newer keyboard session");
    keyboardMenuAction("darkrecomp.keyboard.bind.8.0");
    check(keyboardMenuCaptureActive(), "new keyboard session was left closing by an old host report");
    page(owner, "options_controller", 1);
    updateKeyboardMenuGuest(call, base, owner);
    check(!keyboardMenuCaptureActive(), "ordinary Back did not cancel an active binding capture");
    beginKeyboardMenu(defaultKeyboardBindings());
    page(owner, "darkrecomp_keyboard_1");
    updateKeyboardMenuGuest(call, base, owner);
    keyboardMenuAction("darkrecomp.keyboard.bind.8.0");
    check(keyboardMenuCaptureActive(), "lost-root fixture did not establish capture ownership");
    PPC_STORE_U32(owner + 16, 0);
    updateKeyboardMenuGuest(call, base, owner);
    check(!keyboardMenuCaptureActive(), "released FrontEnd root left an invisible keyboard capture active");
    puts("Keyboard menu guest entry ABI, page ownership, deferred close and stale Save lifecycle passed.");
}
