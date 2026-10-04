#include "keyboard_menu_guest.h"
#include "keyboard_menu.h"
#include "input.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <cstring>

namespace DarkRecomp::Native {
namespace {
uint32_t keyboardFrontend = 0;
bool enteredKeyboardPage = false;

std::string_view byteString(uint8_t* base, uint32_t object) {
    const uint32_t data = PPC_LOAD_U32(object + 4);
    if (!data || uint64_t(data) + 130 > PPC_MEMORY_SIZE || (PPC_LOAD_U16(data) & 0x8000)) return {};
    const char* text = reinterpret_cast<const char*>(base + data + 2);
    const auto length = strnlen_s(text, 128);
    return length < 128 ? std::string_view(text, length) : std::string_view{};
}

void openPage(PPCContext& ctx, uint8_t* base, uint32_t button) {
    // Original CubeButton8239EF38 runs a retained CStr through window+308.
    // This uses that same script dispatcher without mutating the live button.
    auto call = ctx;
    call.r1.u64 -= 512;
    const uint32_t raw = call.r1.u32 + 80, text = call.r1.u32 + 384;
    constexpr char script[] = "cg_submenu('darkrecomp_keyboard_1')";
    std::memcpy(base + raw, script, sizeof(script));
    call.r3.u64 = text; call.r4.u64 = raw;
    sub_821F86A8(call, base);
    call.r3.u64 = button; call.r4.u64 = text;
    const auto function = PPC_LOAD_U32(PPC_LOAD_U32(button) + 308);
    PPCSafeIndirect(call, base, function);
    call.r3.u64 = text;
    sub_821F8AD0(call, base);
}
}

bool activateKeyboardMenu(PPCContext& ctx, uint8_t* base, uint32_t button,
                          std::string_view action) {
    if (action == "darkrecomp.keybindings") {
        beginKeyboardMenu(nativeInput().keyboardBindings());
        keyboardFrontend = 0;
        enteredKeyboardPage = false;
        openPage(ctx, base, button);
        return true;
    }
    const bool handled = keyboardMenuAction(action);
    if (handled && keyboardMenuInputBlocked()) nativeInput().suppressMenuActivationKeys();
    return handled;
}

void updateKeyboardMenuGuest(PPCContext& ctx, uint8_t* base, uint32_t frontend) {
    if (!frontend || uint64_t(frontend) + 468 > PPC_MEMORY_SIZE ||
        PPC_LOAD_U32(frontend) != 0x82071A10) return;
    // Original FrontEnd8236C508 uses these retained names and stack indices.
    // cg_switchmenu replaces the name at the same depth, preserving Back.
    const int32_t depth = int32_t(PPC_LOAD_U32(frontend + 464));
    const auto name = depth >= 0 && depth < 10 ? byteString(base, frontend + 384 + uint32_t(depth) * 8) : std::string_view{};
    const auto root = PPC_LOAD_U32(frontend + 16);
    const bool liveRoot = root && uint64_t(root) + 272 <= PPC_MEMORY_SIZE &&
                          !PPC_LOAD_U8(root + 88) && !(PPC_LOAD_U32(root + 84) & 8);
    const bool keyboardPage = liveRoot && (name == "darkrecomp_keyboard_1" || name == "darkrecomp_keyboard_2" ||
                              name == "darkrecomp_keyboard_3" || name == "darkrecomp_keyboard_4");
    if (keyboardPage) {
        if (enteredKeyboardPage && keyboardFrontend != frontend) return;
        keyboardFrontend = frontend;
        enteredKeyboardPage = true;
        if (takeKeyboardMenuCloseRequest()) {
            // Defer destruction through the original pending-menu operation.
            //8237DD38 sets FrontEnd+308=4, +312=1;8237CED0 pops next update.
            auto call = ctx;
            call.r3.u64 = frontend;
            sub_8237DD38(call, base);
        }
    } else if (enteredKeyboardPage && keyboardFrontend == frontend) {
        endKeyboardMenu(); // Ordinary Escape/Back discards staged edits too.
        keyboardFrontend = 0;
        enteredKeyboardPage = false;
    }
}
}
