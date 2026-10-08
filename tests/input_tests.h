// Deterministic host-device fixtures exercise the production guest ABI. Window
// messages below are test input, not a claim of interactive gameplay.
#include "runtime/native/input.h"
#include "runtime/native/keyboard_menu.h"
#include "renderer/engine/prompt_bindings.h"

namespace {
std::array<DWORD, 4> inputStatus{};
std::array<XINPUT_STATE, 4> inputStates{};
std::array<XINPUT_CAPABILITIES, 4> inputCaps{};
XINPUT_VIBRATION inputLastVibration{};
DWORD inputLastUser = 4;
unsigned inputVibrationCalls = 0;
unsigned inputSystemMenuCalls = 0;
ULONGLONG inputTicks = 1000;
ULONGLONG WINAPI fixtureInputTicks() noexcept { return inputTicks; }
DWORD WINAPI fixtureInputState(DWORD user, XINPUT_STATE* state) noexcept {
    if (user >= inputStatus.size() || !state) return ERROR_INVALID_PARAMETER;
    *state = inputStates[user];
    return inputStatus[user];
}
DWORD WINAPI fixtureInputCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) noexcept {
    if (user >= inputStatus.size() || !caps) return ERROR_INVALID_PARAMETER;
    *caps = inputCaps[user];
    return inputStatus[user];
}
DWORD WINAPI fixtureInputVibration(DWORD user, XINPUT_VIBRATION* vibration) noexcept {
    if (user >= inputStatus.size() || !vibration) return ERROR_INVALID_PARAMETER;
    inputLastUser = user;
    inputLastVibration = *vibration;
    ++inputVibrationCalls;
    return inputStatus[user];
}
LRESULT CALLBACK inputTestWindowProc(HWND window, UINT message, WPARAM key, LPARAM detail) {
    if (message == WM_SYSCOMMAND && (key & 0xfff0) == SC_KEYMENU) {
        ++inputSystemMenuCalls;
        return 0;
    }
    auto* input = reinterpret_cast<NativeInput*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (input && input->windowMessage(window, message, key, detail))
        return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ? TRUE : 0;
    return DefWindowProcW(window, message, key, detail);
}
}

#include "input_backend_tests.h"

static void testSinglePlayerInputRouting(PPCContext& ctx) {
    const uint32_t fixture = memory->allocate(4096), output = fixture + 1;
    const uint32_t vibration = fixture + 64, xuid = fixture + 80;
    check(fixture != 0, "Single-player input fixture allocation failed");
    auto* base = memory->base();
    const auto zero = [&](uint32_t address, unsigned length) {
        return std::all_of(base + address, base + address + length, [](uint8_t value) { return value == 0; });
    };
    // Windows device indices are transport details. Every supported host slot
    // must drive the one guest user that owns the local profile and saves.
    for (uint32_t host = 0; host < XUSER_MAX_COUNT; ++host) {
        inputStatus.fill(ERROR_DEVICE_NOT_CONNECTED); inputStates = {}; inputCaps = {};
        inputStatus[host] = ERROR_SUCCESS;
        inputStates[host].Gamepad.wButtons = XINPUT_GAMEPAD_START;
        inputCaps[host] = {XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD, XINPUT_CAPS_FFB_SUPPORTED,
            {0xf3ff, 255, 255, -1, -1, -1, -1}, {WORD(0x1200 + host), WORD(0xab00 + host)}};
        NativeInput input({fixtureInputState, fixtureInputCapabilities, fixtureInputVibration, fixtureInputTicks});
        PPC_STORE_U16(vibration, 0x1234); PPC_STORE_U16(vibration + 2, 0xabcd);
        // Capabilities and vibration may be queried before the first state.
        if (host == 2)
            check(input.getCapabilities(*memory, 0, XINPUT_FLAG_GAMEPAD, output) == ERROR_SUCCESS &&
                  PPC_LOAD_U16(output + 16) == 0x1200 + host, "Capabilities-first routing chose the wrong host pad");
        if (host == 3)
            check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastUser == host,
                  "Vibration-first routing chose the wrong host pad");
        check(input.getState(*memory, 0, 0, output) == ERROR_SUCCESS &&
              PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_START,
              "Host controller Start did not reach guest user zero");
        check(input.promptSource() == PromptInputSource::Controller,
              "Routed host controller did not select controller prompts");
        ctx.r3.u64 = 0; __imp__XamUserGetSigninState(ctx, base);
        check(ctx.r3.u32 == 1, "Routed input user did not own the signed-in local profile");
        ctx.r3.u64 = 0; ctx.r4.u64 = 7; ctx.r5.u64 = xuid;
        __imp__XamUserGetXUID(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(xuid) == 0xe0000000u && memory->read32(xuid + 4) == 1,
              "Routed input user did not own the stable local XUID");
        check(input.getCapabilities(*memory, 0, XINPUT_FLAG_GAMEPAD, output) == ERROR_SUCCESS &&
              PPC_LOAD_U16(output + 16) == 0x1200 + host && PPC_LOAD_U16(output + 18) == 0xab00 + host,
              "Routed guest capabilities belong to another host pad");
        check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastUser == host &&
              inputLastVibration.wLeftMotorSpeed == 0x1234 && inputLastVibration.wRightMotorSpeed == 0xabcd,
              "Guest user zero rumble did not reach the selected host pad");
        for (uint32_t guest = 1; guest < XUSER_MAX_COUNT; ++guest) {
            memset(base + output, 0xa5, 20);
            check(input.getState(*memory, guest, 0, output) == ERROR_DEVICE_NOT_CONNECTED && zero(output, 16),
                  "A host pad exposed a guest user without a local profile");
            memset(base + output, 0xa5, 20);
            check(input.getCapabilities(*memory, guest, 0, output) == ERROR_DEVICE_NOT_CONNECTED && zero(output, 20),
                  "Secondary guest capabilities exposed an unsigned-in device");
            const unsigned calls = inputVibrationCalls;
            check(input.setState(*memory, guest, 0, vibration) == ERROR_DEVICE_NOT_CONNECTED &&
                  inputVibrationCalls == calls, "Secondary guest rumble reached a host pad");
            ctx.r3.u64 = guest; __imp__XamUserGetSigninState(ctx, base);
            check(ctx.r3.u32 == 0, "Secondary guest user unexpectedly signed in");
            ctx.r3.u64 = guest; ctx.r4.u64 = 7; ctx.r5.u64 = xuid;
            __imp__XamUserGetXUID(ctx, base);
            check(ctx.r3.u32 == 0x80070525u && zero(xuid, 8), "Secondary guest user unexpectedly owned a XUID");
        }
    }
    {
        inputStatus.fill(ERROR_DEVICE_NOT_CONNECTED); inputStates = {}; inputCaps = {};
        inputStatus[2] = ERROR_SUCCESS;
        inputStates[2].Gamepad.wButtons = XINPUT_GAMEPAD_START;
        inputCaps[2].Type = XINPUT_DEVTYPE_GAMEPAD; inputCaps[2].Vibration.wLeftMotorSpeed = 0x2222;
        NativeInput input({fixtureInputState, fixtureInputCapabilities, fixtureInputVibration, fixtureInputTicks});
        check(input.getState(*memory, 0, 0, output) == ERROR_SUCCESS, "Initial routed pad was not discovered");
        const uint32_t packet = memory->read32(output);
        inputStatus[0] = inputStatus[3] = ERROR_SUCCESS;
        inputStates[0].Gamepad.wButtons = XINPUT_GAMEPAD_B;
        inputStates[3].Gamepad.wButtons = XINPUT_GAMEPAD_Y;
        inputCaps[0].Type = XINPUT_DEVTYPE_GAMEPAD; inputCaps[0].Vibration.wLeftMotorSpeed = 0x1111;
        check(input.getState(*memory, 0, 0, output) == ERROR_SUCCESS && memory->read32(output) == packet &&
              PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_START,
              "An additional host pad stole the current guest controller");
        check(input.getCapabilities(*memory, 0, 0, output) == ERROR_SUCCESS && PPC_LOAD_U16(output + 16) == 0x2222,
              "Additional host pad changed the selected capabilities");
        check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastUser == 2,
              "Additional host pad changed the rumble destination");
        inputStatus[2] = ERROR_GEN_FAILURE;
        check(input.getState(*memory, 0, 0, output) == ERROR_GEN_FAILURE && zero(output, 16),
              "A transient host error silently rebound the guest controller");
        inputStatus[2] = ERROR_DEVICE_NOT_CONNECTED;
        check(input.getState(*memory, 0, 0, output) == ERROR_SUCCESS && memory->read32(output) > packet &&
              PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_B,
              "Selected host disconnect did not route the next connected pad");
        check(inputLastUser == 2 && inputLastVibration.wLeftMotorSpeed == 0 && inputLastVibration.wRightMotorSpeed == 0,
              "Controller replacement did not stop the previous host rumble");
        check(input.getCapabilities(*memory, 0, 0, output) == ERROR_SUCCESS && PPC_LOAD_U16(output + 16) == 0x1111 &&
              input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastUser == 0,
              "Replacement state, capabilities and rumble did not share one host route");
    }
    memory->release(fixture);
    puts("Single-player input: host slots zero through three, profile identity, capabilities, rumble and stable replacement passed.");
}

static void testInputContract(PPCContext& ctx) {
    testBackgroundInputContract();
    testSinglePlayerInputRouting(ctx);
    inputStatus.fill(ERROR_DEVICE_NOT_CONNECTED);
    inputStates = {}; inputCaps = {};
    inputVibrationCalls = 0;
    NativeInput input({fixtureInputState, fixtureInputCapabilities, fixtureInputVibration, fixtureInputTicks});
    uint32_t storage = memory->allocate(8192);
    check(storage != 0, "Input fixture allocation failed");
    auto* base = memory->base();
    uint32_t output = storage + 1; // also exercise an unaligned guest buffer
    uint32_t vibration = storage + 64;
    auto state = [&](uint32_t user = 0) { return input.getState(*memory, user, 0, output); };
    auto zero = [&](uint32_t address, unsigned length) {
        return std::all_of(base + address, base + address + length, [](uint8_t b) { return b == 0; });
    };
    memset(base + storage, 0xa5, 128);
    check(state() == ERROR_DEVICE_NOT_CONNECTED && zero(output, 16), "Absent host input was reported connected");
    check(base[storage] == 0xa5 && base[output + 16] == 0xa5, "Input state overwrote a canary");
    inputStatus[0] = ERROR_SUCCESS;
    inputStates[0].dwPacketNumber = 0x12345678;
    inputStates[0].Gamepad = {0xa102, 0x12, 0xef, -32768, 32767, -12345, 23456};
    const uint8_t expected[] = {0,0,0,1, 0xa1,0x02,0x12,0xef, 0x80,0,0x7f,0xff,0xcf,0xc7,0x5b,0xa0};
    check(state() == ERROR_SUCCESS && memcmp(base + output, expected, sizeof(expected)) == 0,
          "Native controller fields did not serialize to exact big-endian bytes");
    check(state() == ERROR_SUCCESS && memory->read32(output) == 1, "Unchanged input advanced its packet");
    ++inputStates[0].dwPacketNumber;
    check(state() == ERROR_SUCCESS && memory->read32(output) == 1, "Host packet bookkeeping invented input changes");
    inputStates[0].Gamepad.wButtons ^= XINPUT_GAMEPAD_A;
    check(state() == ERROR_SUCCESS && memory->read32(output) == 2, "Changed input did not advance its packet");
    inputStatus[0] = ERROR_DEVICE_NOT_CONNECTED;
    check(state() == ERROR_DEVICE_NOT_CONNECTED && zero(output, 16), "Disconnect retained held input");
    inputStatus[0] = ERROR_SUCCESS;
    check(state() == ERROR_SUCCESS && memory->read32(output) == 3, "Reconnect was hidden by a stale packet");

    inputCaps[0] = {XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD, XINPUT_CAPS_FFB_SUPPORTED,
                    {0xf3ff,255,255,-1,-1,-1,-1}, {0x1234,0xabcd}};
    memset(base + output, 0xa5, 24);
    const uint8_t expectedCaps[] = {1,1,0,1, 0xf3,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
                                   0xff,0xff,0xff,0xff, 0x12,0x34,0xab,0xcd};
    check(input.getCapabilities(*memory, 0, XINPUT_FLAG_GAMEPAD, output) == ERROR_SUCCESS &&
          memcmp(base + output, expectedCaps, 20) == 0 && base[output+20] == 0xa5,
          "Capabilities layout, size or vibration flags are wrong");
    const uint8_t motors[] = {0x12,0x34,0xab,0xcd};
    memcpy(base + vibration, motors, 4);
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastUser == 0 &&
          inputLastVibration.wLeftMotorSpeed == 0x1234 && inputLastVibration.wRightMotorSpeed == 0xabcd,
          "Motor values were not sent to the native host in the right byte order");

    // Invalid flags, indices, spans and permissions never reach unsafe stores.
    check(input.getState(*memory, 4, 0, output) == ERROR_INVALID_PARAMETER, "Invalid user accepted");
    check(input.getState(*memory, 0, 1, output) == ERROR_INVALID_PARAMETER, "Unknown state flags accepted");
    check(input.getCapabilities(*memory, 0, 2, output) == ERROR_INVALID_PARAMETER, "Unknown capabilities flags accepted");
    check(input.setState(*memory, 0, 1, vibration) == ERROR_INVALID_PARAMETER, "Unknown vibration flags accepted");
    check(input.getState(*memory, 0, 0, 0) == ERROR_INVALID_PARAMETER, "Null state output accepted");
    check(input.getCapabilities(*memory, 0, 0, 0) == ERROR_INVALID_PARAMETER, "Null caps output accepted");
    check(input.setState(*memory, 0, 0, 0) == ERROR_INVALID_PARAMETER, "Null motor input accepted");
    check(input.getState(*memory, 0, 0, 0xfffffff8) == ERROR_INVALID_PARAMETER, "Wrapped output accepted");
    DWORD oldProtect = 0;
    check(VirtualProtect(base + storage + 4096, 4096, PAGE_NOACCESS, &oldProtect) != 0, "Cannot protect input fixture");
    check(input.getState(*memory, 0, 0, storage + 4090) == ERROR_INVALID_PARAMETER, "Cross-page output reached an inaccessible page");
    check(input.setState(*memory, 0, 0, storage + 4094) == ERROR_INVALID_PARAMETER, "Cross-page motor input reached an inaccessible page");
    check(VirtualProtect(base + storage + 4096, 4096, PAGE_READONLY, &oldProtect) != 0, "Cannot make input fixture read-only");
    check(input.getState(*memory, 0, 0, storage + 4090) == ERROR_INVALID_PARAMETER, "Read-only output accepted");
    memset(base + storage + 4094, 0, 2);
    check(input.setState(*memory, 0, 0, storage + 4094) == ERROR_SUCCESS, "Readable motor input crossing regions rejected");
    check(VirtualProtect(base + storage + 4096, 4096, PAGE_READWRITE, &oldProtect) != 0, "Cannot restore input fixture");

    WNDCLASSW wc{};
    wc.lpfnWndProc = inputTestWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DarkRecompInputContract";
    check(RegisterClassW(&wc) != 0, "Cannot register input test window");
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"Input contract", 0, 0,0,0,0,
                                 HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    check(window != nullptr, "Cannot create input test window");
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&input));
    input.attachWindow(window);
    inputStatus.fill(ERROR_DEVICE_NOT_CONNECTED);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    check(input.guestMenuAllowsPointer(), "Unknown startup client must permit observed menu roots");
    check(!input.menuCursor().valid, "Menu pointer must be inert before its first position");
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(25, 50));
    const auto firstPointer = input.menuCursor();
    check(firstPointer.valid && firstPointer.x == 25 && firstPointer.y == 50,
          "Menu pointer must preserve client coordinates");
    SendMessageW(window, WM_LBUTTONDOWN, 0, MAKELPARAM(120, 110));
    const auto pressedPointer = input.menuCursor();
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(120, 110));
    const auto releasedPointer = input.menuCursor();
    check(pressedPointer.leftDown && pressedPointer.presses == firstPointer.presses + 1 &&
          releasedPointer.valid && !releasedPointer.leftDown && releasedPointer.presses == pressedPointer.presses &&
          releasedPointer.x == 120 && releasedPointer.y == 110,
          "Quick click must retain one press and its own client coordinates after release");
    input.setMouseLookEnabled(true);
    check(!input.menuCursor().valid && input.menuCursor().epoch != releasedPointer.epoch,
          "Gameplay capture must invalidate pointer ownership and stale clicks");
    input.setMouseLookEnabled(false);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Attached keyboard did not provide a neutral controller");
    uint32_t packet = memory->read32(output);
    // Initial menus have no client/GUI signal yet. Keep their released wheel
    // navigation, then discard every queued detent at the first gameplay signal.
    input.setMouseLookEnabled(false);
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,short(-240)),0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_DOWN,"Startup menu wheel fallback was lost");
    input.setGuestMenuActive(false);state();
    check(!input.guestMenuAllowsPointer(), "Known gameplay must reject HUD roots as menus");
    check(PPC_LOAD_U16(output+4)==0,"First gameplay signal replayed startup menu wheel input");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);state();
    check(PPC_LOAD_U16(output+4)==0,"Released gameplay wheel changed a power after the first guest signal");
    inputTicks+=100;state();
    check(PPC_LOAD_U16(output+4)==0,"Released gameplay wheel retained a queued startup detent");
    packet=memory->read32(output);
    input.setMouseLookEnabled(true);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    SendMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, 0);
    check(state() == ERROR_SUCCESS && memory->read32(output) == packet + 1 &&
          PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_START && base[output + 6] == 255 &&
          PPC_LOAD_U16(output + 10) == 32767, "Window messages did not reach Start, movement and trigger input");
    packet = memory->read32(output);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, LPARAM(1) << 30);
    check(state() == ERROR_SUCCESS && memory->read32(output) == packet, "Keyboard autorepeat invented input changes");
    SendMessageW(window, WM_KEYDOWN, 'S', 0);
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 10) == 0, "Opposite movement keys did not cancel");
    SendMessageW(window, WM_KEYUP, 'S', 0);
    SendMessageW(window, WM_KEYUP, 'W', 0);
    SendMessageW(window, WM_KEYUP, VK_RETURN, 0);
    SendMessageW(window, WM_CAPTURECHANGED, 0, 0);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Key release or lost mouse capture retained held input");
    SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_A, "Space did not map to A");
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    SendMessageW(window, WM_KEYDOWN, 'D', 0);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Background window received input");
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Focus regain restored stale keys");
    check(state(1) == ERROR_DEVICE_NOT_CONNECTED, "Keyboard fabricated an extra player");
    check(input.getCapabilities(*memory, 0, 0, output) == ERROR_SUCCESS &&
          PPC_LOAD_U16(output + 2) == 0 && zero(output + 16, 4), "Keyboard advertised nonexistent rumble hardware");
    memcpy(base + vibration, motors, 4);
    check(input.setState(*memory, 0, 0, vibration) == ERROR_NOT_SUPPORTED, "Keyboard pretended to vibrate");
    memset(base + vibration, 0, 4);
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS, "Neutral keyboard vibration request failed");

    // Correct original game actions, including distinct menu/gameplay Space.
    const std::pair<unsigned, WORD> bindings[]{
        {'E',XINPUT_GAMEPAD_A},{'R',XINPUT_GAMEPAD_B},{'F',XINPUT_GAMEPAD_X},
        {'Q',XINPUT_GAMEPAD_LEFT_SHOULDER},{'G',XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {VK_CONTROL,XINPUT_GAMEPAD_LEFT_THUMB},{VK_SHIFT,XINPUT_GAMEPAD_RIGHT_THUMB},
        {'1',XINPUT_GAMEPAD_DPAD_LEFT},{'2',XINPUT_GAMEPAD_DPAD_RIGHT},
        {'3',XINPUT_GAMEPAD_DPAD_UP},{'4',XINPUT_GAMEPAD_DPAD_DOWN}};
    for (auto [key, button] : bindings) {
        SendMessageW(window, WM_KEYDOWN, key, 0);
        check(state()==ERROR_SUCCESS && PPC_LOAD_U16(output+4)==button,"Native action key uses wrong game binding");
        SendMessageW(window, WM_KEYUP, key, 0);
    }
    input.setMouseLookEnabled(true);
    SendMessageW(window,WM_KEYDOWN,VK_SPACE,0);
    check(state()==ERROR_SUCCESS && PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_Y,"Captured Space did not jump");
    SendMessageW(window,WM_KEYUP,VK_SPACE,0);
    SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0);
    input.setMouseLookEnabled(false); // Window thread releases capture after Pause.
    check(state()==ERROR_SUCCESS && PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_START,"Mouse release swallowed Escape Pause");
    SendMessageW(window,WM_KEYUP,VK_ESCAPE,0);
    SendMessageW(window,WM_KEYDOWN,VK_ESCAPE,0);
    check(state()==ERROR_SUCCESS && PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_B,"Menu Escape did not cancel");
    SendMessageW(window,WM_KEYUP,VK_ESCAPE,0);

    // A saved selection feeds the production guest ABI, removes former keys,
    // and leaves fixed menu navigation usable after gameplay keys are unbound.
    const auto defaultMenuLabels = DarkRecomp::Prompts::bindingLabels();
    const auto defaultGameplayLabels = DarkRecomp::Prompts::bindingLabels(true);
    auto customKeys = defaultKeyboardBindings();
    check(assignKeyboardKey(customKeys, KeyboardAction::MoveForward, 0, VK_UP) &&
          assignKeyboardKey(customKeys, KeyboardAction::Jump, 0, 'P') &&
          assignKeyboardKey(customKeys, KeyboardAction::Use, 0, 'U') &&
          assignKeyboardKey(customKeys, KeyboardAction::Reload, 0, 'T') && input.setKeyboardBindings(customKeys),
          "Cannot apply custom keyboard selection");
    const auto remappedMenuLabels = DarkRecomp::Prompts::bindingLabels();
    const auto remappedGameplayLabels = DarkRecomp::Prompts::bindingLabels(true);
    check((*remappedMenuLabels)[0] == "U/E" && (*remappedGameplayLabels)[0] == "U" &&
          (*remappedMenuLabels)[1] == "T/Esc" && (*remappedGameplayLabels)[1] == "T" &&
          (*remappedMenuLabels)[3] == "P/Spc" && (*remappedGameplayLabels)[3] == "P" &&
          (*remappedGameplayLabels)[9] == "Sh/MMB",
          "Gameplay prompts lost configured actions or inherited fixed menu alternatives");
    check((*defaultMenuLabels)[1] == "R/Esc" && (*defaultGameplayLabels)[1] == "R",
          "Rebinding changed an owned prior prompt snapshot");
    input.setMouseLookEnabled(true);
    SendMessageW(window,WM_KEYDOWN,'W',0);SendMessageW(window,WM_KEYDOWN,'E',0);state();
    check(zero(output+4,12), "Former movement or use key remained bound in gameplay");
    SendMessageW(window,WM_KEYUP,'W',0);SendMessageW(window,WM_KEYUP,'E',0);
    SendMessageW(window,WM_KEYDOWN,VK_UP,0);state();
    check(PPC_LOAD_U16(output+10)==32767 && PPC_LOAD_U16(output+4)==0,
          "Custom arrow movement switched a Darkness power");
    SendMessageW(window,WM_KEYUP,VK_UP,0);
    SendMessageW(window,WM_KEYDOWN,'P',0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_Y,"Remapped jump did not reach the original action");
    SendMessageW(window,WM_KEYUP,'P',0);SendMessageW(window,WM_KEYDOWN,'U',0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_A,"Remapped use did not reach the original action");
    SendMessageW(window,WM_KEYUP,'U',0);SendMessageW(window,WM_KEYDOWN,'T',0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_B,
          "Reload prompt key did not reach the original gameplay B action");
    auto invalidKeys = customKeys; invalidKeys.keys[size_t(KeyboardAction::Use)][0]=VK_ESCAPE;
    check(!input.setKeyboardBindings(invalidKeys) && input.keyboardBindings()==customKeys,
          "Invalid runtime mapping replaced valid controls");
    check(DarkRecomp::Prompts::bindingLabels() == remappedMenuLabels &&
          DarkRecomp::Prompts::bindingLabels(true) == remappedGameplayLabels,
          "Invalid binding changed a published prompt snapshot");
    auto secondaryReload = customKeys;
    check(assignKeyboardKey(secondaryReload, KeyboardAction::Reload, 0, 0) &&
          assignKeyboardKey(secondaryReload, KeyboardAction::Reload, 1, 'T') &&
          input.setKeyboardBindings(secondaryReload), "Cannot select secondary-only reload binding");
    check((*DarkRecomp::Prompts::bindingLabels())[1] == "T/Esc" &&
          (*DarkRecomp::Prompts::bindingLabels(true))[1] == "T",
          "Secondary-only reload prompt showed the former primary key");
    input.setMouseLookEnabled(true);
    SendMessageW(window,WM_KEYUP,'T',0);SendMessageW(window,WM_KEYDOWN,'T',0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_B,
          "Secondary reload prompt key did not reach gameplay B");
    check(input.setKeyboardBindings(KeyboardBindings{}),"Cannot unbind gameplay keys");state();
    check(zero(output+4,12),"Changing controls retained the formerly held action");
    check((*DarkRecomp::Prompts::bindingLabels())[1] == "-/Esc" &&
          (*DarkRecomp::Prompts::bindingLabels(true))[1] == "-" &&
          (*remappedGameplayLabels)[1] == "T",
          "Unbound reload prompt retained Escape or changed an owned remapped snapshot");
    SendMessageW(window,WM_KEYDOWN,'E',0);SendMessageW(window,WM_KEYDOWN,VK_UP,0);state();
    check(PPC_LOAD_U16(output+4)==(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_DPAD_UP),
          "Unbound gameplay settings removed fixed menu navigation");
    check(input.setKeyboardBindings(defaultKeyboardBindings()),"Cannot restore keyboard defaults");
    check(*DarkRecomp::Prompts::bindingLabels() == DarkRecomp::Prompts::defaultBindingLabels() &&
          *DarkRecomp::Prompts::bindingLabels(true) == DarkRecomp::Prompts::defaultGameplayBindingLabels(),
          "Restored controls did not restore both prompt contexts");

    auto menuConflict = defaultKeyboardBindings();
    check(assignKeyboardKey(menuConflict, KeyboardAction::Reload, 0, 'E') &&
          assignKeyboardKey(menuConflict, KeyboardAction::RedirectDarkling, 0, VK_UP) &&
          input.setKeyboardBindings(menuConflict), "Cannot configure fixed-menu key conflict fixture");
    SendMessageW(window,WM_KEYDOWN,'E',0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_A,"Fixed menu E delivered both confirm and remapped reload");
    SendMessageW(window,WM_KEYUP,'E',0);SendMessageW(window,WM_KEYDOWN,VK_UP,0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_UP,"Fixed menu arrow delivered remapped gameplay action");
    SendMessageW(window,WM_KEYUP,VK_UP,0);
    input.setGuestMenuActive(true);input.setMouseLookEnabled(true);
    check(input.guestMenuAllowsPointer(), "Observed active menu must replace stale gameplay ownership");
    SendMessageW(window,WM_KEYDOWN,'E',0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_A,"Guest menu with capture delivered conflicting gameplay action");
    check(input.setKeyboardBindings(defaultKeyboardBindings()),"Cannot restore keyboard controls after menu conflict test");
    input.setGuestMenuActive(false);

    // Mouse assignments must replace the fixed fire/zoom mappings and support
    // side buttons through the same Win32 -> guest ABI as keyboard controls.
    auto mouseBindings = defaultKeyboardBindings();
    check(assignKeyboardKey(mouseBindings, KeyboardAction::Jump, 1, VK_LBUTTON) &&
          assignKeyboardKey(mouseBindings, KeyboardAction::Reload, 1, VK_RBUTTON) &&
          assignKeyboardKey(mouseBindings, KeyboardAction::Use, 1, VK_MBUTTON) &&
          assignKeyboardKey(mouseBindings, KeyboardAction::Crouch, 1, VK_XBUTTON1) &&
          assignKeyboardKey(mouseBindings, KeyboardAction::MoveForward, 1, VK_XBUTTON2) &&
          input.setKeyboardBindings(mouseBindings), "Cannot configure five mouse buttons");
    check((*DarkRecomp::Prompts::bindingLabels(true))[6] == "Z" &&
          (*DarkRecomp::Prompts::bindingLabels(true))[7] == "X" &&
          (*DarkRecomp::Prompts::bindingLabels(true))[9] == "Sh",
          "Remapped fire/zoom prompts retained the former mouse buttons");
    input.setMouseLookEnabled(true);
    const struct { UINT press, release; WPARAM parameter; WORD button; SHORT forward; } mouseActions[] = {
        {WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON, XINPUT_GAMEPAD_Y, 0},
        {WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, XINPUT_GAMEPAD_B, 0},
        {WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON, XINPUT_GAMEPAD_A, 0},
        {WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), XINPUT_GAMEPAD_LEFT_THUMB, 0},
        {WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), 0, 32767}
    };
    for (const auto& action : mouseActions) {
        SendMessageW(window, action.press, action.parameter, 0); state();
        check(PPC_LOAD_U16(output + 4) == action.button && int16_t(PPC_LOAD_U16(output + 10)) == action.forward &&
              base[output + 6] == 0 && base[output + 7] == 0,
              "Remapped mouse action also fired or zoomed through its old mapping");
        SendMessageW(window, action.release, action.parameter, 0); state();
        check(zero(output + 4, 12), "Mouse binding release retained the action");
    }
    input.setGuestMenuActive(true);
    const auto beforeMouseMenu = input.menuCursor();
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(125, 110)); state();
    check(zero(output + 4, 12) && input.menuCursor().presses == beforeMouseMenu.presses + 1,
          "Menu click activated a remapped gameplay action or lost pointer selection");
    SendMessageW(window, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0); state();
    check(zero(output + 4, 12), "Captured guest menu delivered a mouse gameplay binding");
    SendMessageW(window, WM_LBUTTONUP, 0, 0);
    SendMessageW(window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    input.setGuestMenuActive(false);
    SendMessageW(window, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), 0);
    SendMessageW(window, WM_CAPTURECHANGED, 0, 0); state();
    input.setMouseLookEnabled(true); state();
    check(zero(output + 4, 12), "Recapture replayed a held side button");
    SendMessageW(window, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    input.setMouseLookEnabled(true); state();
    check(zero(output + 4, 12), "Focus regain replayed a held side button");
    SendMessageW(window, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    check(input.setKeyboardBindings(KeyboardBindings{}), "Cannot clear mouse controls");
    input.setMouseLookEnabled(true);
    for (const auto& action : mouseActions) {
        SendMessageW(window, action.press, action.parameter, 0); state();
        check(zero(output + 4, 12), "Explicitly unbound mouse button retained a fixed action");
        SendMessageW(window, action.release, action.parameter, 0);
    }
    check(input.setKeyboardBindings(defaultKeyboardBindings()), "Cannot restore mouse defaults");

    auto altBindings = defaultKeyboardBindings();
    check(assignKeyboardKey(altBindings, KeyboardAction::Jump, 1, VK_MENU) &&
          input.setKeyboardBindings(altBindings), "Cannot configure Alt gameplay binding");
    input.setMouseLookEnabled(true);
    const auto beforeAltMenu = inputSystemMenuCalls;
    constexpr LPARAM altContext = LPARAM(1) << 29;
    SendMessageW(window, WM_SYSKEYDOWN, VK_MENU, altContext); state();
    check(PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_Y,
          "Bare Alt system key did not deliver the configured action");
    SendMessageW(window, WM_SYSKEYDOWN, 'W', altContext); state();
    check(PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_Y && int16_t(PPC_LOAD_U16(output + 10)) == 32767,
          "Holding Alt prevented ordinary gameplay movement");
    SendMessageW(window, WM_SYSKEYUP, 'W', altContext);
    SendMessageW(window, WM_SYSKEYUP, VK_MENU, altContext); state();
    check(zero(output + 4, 12) && inputSystemMenuCalls == beforeAltMenu,
          "Bound Alt release stuck the action or activated the host system menu");
    for (const auto shortcut : {VK_RETURN, VK_F4, VK_SPACE, VK_TAB}) {
        SendMessageW(window, WM_SYSKEYDOWN, VK_MENU, altContext);
        check(!input.windowMessage(window, WM_SYSKEYDOWN, shortcut, altContext),
              "Alt binding consumed a reserved host shortcut");
        state();
        check(zero(output + 4, 12), "Host shortcut retained the bound Alt or chord action");
        input.windowMessage(window, WM_SYSKEYUP, shortcut, altContext);
        SendMessageW(window, WM_SYSKEYDOWN, VK_MENU, altContext | (LPARAM(1) << 30)); state();
        check(zero(output + 4, 12), "Alt repeat replayed its action after a host shortcut");
        SendMessageW(window, WM_SYSKEYUP, VK_MENU, altContext); state();
    }
    SendMessageW(window, WM_SYSKEYDOWN, VK_MENU, altContext);
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    input.setMouseLookEnabled(true); state();
    check(zero(output + 4, 12), "Focus regain replayed a held Alt binding");
    SendMessageW(window, WM_SYSKEYUP, VK_MENU, altContext);
    check(input.setKeyboardBindings(defaultKeyboardBindings()), "Cannot restore defaults after Alt checks");

    auto rx=[&] {return int16_t(PPC_LOAD_U16(output+12));};
    auto ry=[&] {return int16_t(PPC_LOAD_U16(output+14));};
    input.mouseMotion(100,100);inputTicks+=20;
    check(state()==ERROR_SUCCESS && rx()==0 && ry()==0 && input.consumeMouseLook().x==0,
          "Released mouse moved the camera");
    input.setMouseLookEnabled(true);
    input.mouseMotion(8,-4);inputTicks+=20;
    check(state()==ERROR_SUCCESS && rx()==0 && ry()==0,"Raw mouse motion leaked into emulated right stick");
    packet=memory->read32(output);
    for(unsigned i=0;i<20;++i) { inputTicks+=1; state(); }
    check(memory->read32(output)==packet,"Mouse movement invented an XInput packet change");
    const auto combined=input.consumeMouseLook();
    check(combined.x==8 && combined.y==-4 && combined.sensitivity==1,
          "SDK polls consumed or rescaled relative mouse counts");
    const auto stopped=input.consumeMouseLook();
    check(stopped.x==0 && stopped.y==0,"Stopped mouse replayed a previous look batch");
    for(unsigned i=0;i<4;++i)input.mouseMotion(2,-1);
    inputTicks+=200;state();
    const auto batched=input.consumeMouseLook();
    check(batched.x==combined.x && batched.y==combined.y,"HID batching or elapsed time changed mouse distance");
    input.mouseMotion(-8,4);
    const auto reversed=input.consumeMouseLook();
    check(reversed.x==-combined.x && reversed.y==-combined.y,"Reverse mouse direction is asymmetric");
    input.mouseMotion(1,0);
    check(input.consumeMouseLook().x==1,"One-count mouse motion disappeared in a dead zone");
    input.mouseMotion(LONG_MAX,LONG_MIN);input.mouseMotion(LONG_MAX,LONG_MIN);
    const auto extreme=input.consumeMouseLook();
    check(extreme.x==int64_t(LONG_MAX)*2 && extreme.y==int64_t(LONG_MIN)*2,
          "Fast mouse motion overflowed or hit a stick speed limit");
    for(unsigned frames:{30u,60u,120u}) {
        int64_t totalX=0,totalY=0;
        for(unsigned frame=0;frame<frames;++frame) {
            input.mouseMotion(LONG(1200/frames),LONG(-600/int(frames)));
            inputTicks+=1000/frames;state();state();
            const auto delta=input.consumeMouseLook();totalX+=delta.x;totalY+=delta.y;
        }
        check(totalX==1200 && totalY==-600,"Frame or SDK polling rate changed mouse distance");
    }
    check(!input.setMouseSensitivity(0) && !input.setMouseSensitivity(INFINITY) &&
          !input.setMouseSensitivity(NAN) && input.setMouseSensitivity(2),"Invalid mouse sensitivity accepted");
    input.mouseMotion(8,-4);
    const auto sensitive=input.consumeMouseLook();
    check(sensitive.x==8 && sensitive.y==-4 && sensitive.sensitivity==2,"Mouse sensitivity lost or applied to device counts");
    check(input.setMouseSensitivity(1),"Cannot restore mouse sensitivity");
    inputStatus[0]=ERROR_SUCCESS;
    inputStates[0].Gamepad.sThumbRX=23456;inputStates[0].Gamepad.sThumbRY=-12345;
    input.mouseMotion(8,-4);state();
    check(rx()==23456 && ry()==-12345,"Raw mouse overrode physical controller look");
    check(input.consumeMouseLook().x==8,"Physical controller poll swallowed mouse movement");
    inputStatus[0]=ERROR_DEVICE_NOT_CONNECTED;inputStates[0]={};
    input.mouseMotion(50,50);input.setMouseLookEnabled(false);input.setMouseLookEnabled(true);
    check(input.consumeMouseLook().x==0,"Recapture replayed old relative motion");

    input.setMouseLookEnabled(true);
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,60),0);state();
    check(PPC_LOAD_U16(output+4)==0,"Partial wheel detent switched a weapon");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,180),0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_RIGHT,"Wheel did not select next weapon");
    inputTicks+=40;state();check(PPC_LOAD_U16(output+4)==0,"Wheel lacked a release edge");
    inputTicks+=20;state();check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_RIGHT,"Second wheel detent was lost");
    input.setGuestMenuActive(true);
    state();check(PPC_LOAD_U16(output+4)==0,"Opening dialogue replayed a gameplay wheel press");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,60),0);state();
    check(PPC_LOAD_U16(output+4)==0,"Partial dialogue wheel detent changed a choice");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,180),0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_UP,"Captured dialogue wheel did not select the previous choice");
    inputTicks+=40;state();check(PPC_LOAD_U16(output+4)==0,"Dialogue wheel lacked a release edge");
    inputTicks+=20;state();check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_UP,"Second dialogue wheel detent was lost");
    input.setGuestMenuActive(false);state();
    check(PPC_LOAD_U16(output+4)==0,"Closing dialogue replayed a choice as a weapon switch");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,short(-120)),0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_LEFT,"Dialogue exit did not restore previous-weapon wheel input");
    input.setMouseLookEnabled(false);
    // F2 releases capture without pausing. An established gameplay context
    // must ignore wheel input rather than synthesizing the power-cycle keys.
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,60),0);state();
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,180),0);state();
    check(PPC_LOAD_U16(output+4)==0,"F2 released gameplay wheel changed a power");
    inputTicks+=100;state();
    check(PPC_LOAD_U16(output+4)==0,"F2 released gameplay wheel queued later input");
    input.setGuestMenuActive(true);
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,short(-240)),0);state();
    check(PPC_LOAD_U16(output+4)==XINPUT_GAMEPAD_DPAD_DOWN,"Released menu wheel did not select the next choice");
    inputTicks+=40;state();check(PPC_LOAD_U16(output+4)==0,"Released menu wheel lacked a release edge");
    input.setGuestMenuActive(false);state();
    check(PPC_LOAD_U16(output+4)==0,"Released GUI exit retained queued menu wheel input");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,short(-120)),0);state();
    check(PPC_LOAD_U16(output+4)==0,"Released GUI exit did not restore gameplay wheel suppression");
    input.setMouseLookEnabled(true);state();
    check(PPC_LOAD_U16(output+4)==0,"Recapture replayed menu wheel input as a weapon switch");
    SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,0);
    SendMessageW(window,WM_RBUTTONDOWN,MK_RBUTTON,0);
    SendMessageW(window,WM_MBUTTONDOWN,MK_MBUTTON,0);state();
    check(base[output+6]==255 && base[output+7]==255 &&
          (PPC_LOAD_U16(output+4)&XINPUT_GAMEPAD_RIGHT_THUMB),"Dual fire or mouse zoom missing");
    input.mouseMotion(50,50);
    SendMessageW(window,WM_KILLFOCUS,0,0);inputTicks+=20;state();
    check(zero(output+4,12) && !input.mouseLookEnabled() && input.consumeMouseLook().x==0,
          "Focus loss retained captured mouse input");
    SendMessageW(window,WM_SETFOCUS,0,0);state();
    check(zero(output+4,12),"Focus regain replayed mouse or wheel input");
    SendMessageW(window,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);state();
    check(zero(output+4,12),"Focus regain forgot established gameplay wheel ownership");

    inputStatus[1] = ERROR_SUCCESS;
    inputStates[1].Gamepad = {XINPUT_GAMEPAD_Y, 0, 0, 0, 0, -32768, 32767};
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_Y,
          "Nonzero host controller did not drive the local guest user");
    memcpy(base + vibration, motors, 4);
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastUser == 1, "Wrong host controller vibrated");
    unsigned calls = inputVibrationCalls;
    SendMessageW(window, WM_ACTIVATEAPP, FALSE, 0);
    check(inputVibrationCalls == calls + 1 && inputLastUser == 1 &&
          inputLastVibration.wLeftMotorSpeed == 0 && inputLastVibration.wRightMotorSpeed == 0,
          "Losing focus did not stop native vibration");
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Routed controller was not neutral while unfocused");
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastVibration.wLeftMotorSpeed == 0,
          "Background guest restarted rumble");
    SendMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_Y,
          "Native controller did not resume on focus");
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS, "Routed native vibration could not resume");
    input.setSettingsOpen(true);
    check(inputLastVibration.wLeftMotorSpeed == 0 && inputLastVibration.wRightMotorSpeed == 0,
          "settings did not stop active rumble");
    SendMessageW(window, WM_ACTIVATEAPP, TRUE, 0);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    input.setMouseLookEnabled(true); input.mouseMotion(50, 50);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12) && !input.mouseLookEnabled() &&
          input.consumeMouseLook().x == 0, "settings leaked keyboard or mouse input after focus change");
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "settings leaked routed physical controller input");
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS && inputLastVibration.wLeftMotorSpeed == 0,
          "settings allowed game to restart rumble");
    input.setSettingsOpen(false);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "panel closing replayed held routed controller input");
    inputStates[1].Gamepad = {};
    state();
    inputStates[1].Gamepad.wButtons = XINPUT_GAMEPAD_B;
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_B,
          "controller did not resume after release");
    inputStates[1].Gamepad = {}; state();
    SendMessageW(window, WM_KEYDOWN, 'W', LPARAM(1) << 30);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "panel closing replayed keyboard autorepeat");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 10) == 32767, "keyboard did not resume after release");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    check(input.setState(*memory, 0, 0, vibration) == ERROR_SUCCESS, "rumble did not resume after closing settings");
    calls = inputVibrationCalls;
    input.attachWindow(nullptr);
    check(inputVibrationCalls == calls + 1 && inputLastVibration.wRightMotorSpeed == 0,
          "Input detach left a motor running");
    inputStatus[1] = ERROR_DEVICE_NOT_CONNECTED;
    check(state() == ERROR_DEVICE_NOT_CONNECTED, "Detached keyboard remained connected");

    // Adaptive prompt source: keyboard/mouse default, controller only on new
    // physical activity, keyboard/mouse always wins back. Merged guest bits
    // never count as controller activity.
    input.attachWindow(window);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&input));
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    inputStatus.fill(ERROR_DEVICE_NOT_CONNECTED);
    inputStates = {};
    auto promptState = [&] { return input.getState(*memory, 0, 0, output); };
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Prompt source did not default to keyboard/mouse");
    check(promptState() == ERROR_SUCCESS && zero(output + 4, 12), "Idle keyboard-only poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Neutral keyboard-only poll left keyboard/mouse source");
    SendMessageW(window, WM_KEYDOWN, 'E', 0);
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Keyboard press left keyboard/mouse source");
    check(promptState() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_A,
          "Virtual keyboard was misidentified instead of merged as controller A");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Merged keyboard input read back as physical controller");
    SendMessageW(window, WM_KEYUP, 'E', 0);
    inputStatus[0] = ERROR_SUCCESS;
    inputStates[0] = {};
    check(promptState() == ERROR_SUCCESS, "Idle controller poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Idle controller stole keyboard/mouse prompts");
    inputStates[0].Gamepad.sThumbLX = 1000;
    inputStates[0].Gamepad.sThumbRY = -1000;
    inputStates[0].Gamepad.bLeftTrigger = 10;
    check(promptState() == ERROR_SUCCESS, "Jitter poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Deadzone jitter stole controller prompts");
    ++inputStates[0].dwPacketNumber;
    check(promptState() == ERROR_SUCCESS, "Churn poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Packet churn stole controller prompts");
    inputStates[0].Gamepad = {XINPUT_GAMEPAD_A, 0, 0, 0, 0, 0, 0};
    check(promptState() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_A, "Controller button did not reach guest");
    check(input.promptSource() == PromptInputSource::Controller, "Controller button did not switch prompts");
    check(promptState() == ERROR_SUCCESS, "Held controller poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Held controller lost its own prompts while idle");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Keyboard press did not win prompts back eagerly");
    check(promptState() == ERROR_SUCCESS && PPC_LOAD_U16(output + 10) == 32767, "Keyboard move did not merge over held controller");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Keyboard did not win prompts back on poll");
    check(promptState() == ERROR_SUCCESS, "Simultaneous held poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Held controller stole prompts back from held keyboard");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Keyboard release poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Unchanged held controller stole prompts without new input");
    inputStates[0].Gamepad = {};
    check(promptState() == ERROR_SUCCESS, "Controller release poll failed");
    inputStates[0].Gamepad.bLeftTrigger = 100;
    check(promptState() == ERROR_SUCCESS && base[output + 6] == 100, "Controller trigger did not reach guest");
    check(input.promptSource() == PromptInputSource::Controller, "Controller trigger edge did not switch prompts");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Trigger reset poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Trigger reset did not return to keyboard/mouse");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    inputStates[0].Gamepad = {};
    inputStates[0].Gamepad.sThumbLX = 20000;
    check(promptState() == ERROR_SUCCESS, "Stick poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Over-deadzone stick did not switch prompts");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Mouse press did not win prompts back eagerly");
    SendMessageW(window, WM_LBUTTONUP, MK_LBUTTON, 0);
    input.setMouseLookEnabled(true);
    input.mouseMotion(8, -4);
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Mouse motion did not hold keyboard/mouse prompts");
    inputTicks += 20;
    check(promptState() == ERROR_SUCCESS && int16_t(PPC_LOAD_U16(output + 12)) == 0 &&
          input.consumeMouseLook().x == 8, "Prompt switching swallowed raw mouse look or emulated a stick");
    input.mouseMotion(1,0);input.consumeMouseLook();
    inputStates[0].Gamepad.bRightTrigger=200;
    check(promptState()==ERROR_SUCCESS && input.promptSource()==PromptInputSource::KeyboardMouse,
          "Consuming camera movement erased mouse activity before the controller prompt poll");
    input.setMouseLookEnabled(false);
    inputStatus[0] = ERROR_DEVICE_NOT_CONNECTED;
    inputStates[0] = {};
    check(promptState() == ERROR_SUCCESS, "Post-disconnect keyboard poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Disconnect left stuck controller prompts");
    inputStatus[0] = ERROR_SUCCESS;
    inputStates[0].Gamepad = {XINPUT_GAMEPAD_Y, 0, 0, 0, 0, 0, 0};
    check(promptState() == ERROR_SUCCESS, "Reconnect poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Reconnect edge did not switch prompts");
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Focus loss left stuck controller prompts");
    inputStates[0] = {};
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    check(promptState() == ERROR_SUCCESS && zero(output + 4, 12), "Focus regain replayed input");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Focus regain left controller prompts");
    // Negative-diagonal overflow: both axes at -32768 must count as activity,
    // not wrap to a false inside-deadzone result.
    inputStatus[0] = ERROR_SUCCESS;
    inputStates[0].Gamepad = {0, 0, 0, -32768, -32768, 0, 0};
    check(promptState() == ERROR_SUCCESS, "Negative-diagonal poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Full negative diagonal did not switch prompts");
    // Retrigger while already outside: a meaningful stick move or trigger
    // change after keyboard use must steal back; small jitter must not.
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Diagonal reset poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Diagonal reset did not return to keyboard/mouse");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    inputStates[0].Gamepad.sThumbLX = -32768; inputStates[0].Gamepad.sThumbLY = -32768;
    inputStates[0].Gamepad.sThumbRX = 0; inputStates[0].Gamepad.sThumbRY = 0;
    check(promptState() == ERROR_SUCCESS, "Held-diagonal baseline poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Unchanged held diagonal stole prompts without new input");
    inputStates[0].Gamepad.sThumbLX = 32767; inputStates[0].Gamepad.sThumbLY = 32767;
    check(promptState() == ERROR_SUCCESS, "Diagonal swing poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Meaningful deflected-stick move did not reselect controller");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Swing reset poll failed");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    inputStates[0].Gamepad = {};
    inputStates[0].Gamepad.bLeftTrigger = 100;
    check(promptState() == ERROR_SUCCESS, "Trigger baseline poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Trigger baseline did not switch prompts");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Trigger keyboard reset failed");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    inputStates[0].Gamepad.bLeftTrigger = 105;
    check(promptState() == ERROR_SUCCESS, "Trigger jitter poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Small held-trigger jitter stole prompts");
    inputStates[0].Gamepad.bLeftTrigger = 200;
    check(promptState() == ERROR_SUCCESS, "Trigger swing poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Meaningful held-trigger change did not reselect controller");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Trigger swing reset failed");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    // Gradual analog drift must accumulate from an anchor, not per-poll deltas.
    inputStates[0].Gamepad = {};
    inputStates[0].Gamepad.sThumbLX = 20000;
    check(promptState() == ERROR_SUCCESS, "Anchor baseline poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Anchor baseline did not switch prompts");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Anchor reset poll failed");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    for (int step = 1; step <= 5; ++step) {
        inputStates[0].Gamepad.sThumbLX = SHORT(20000 + step * 1000);
        check(promptState() == ERROR_SUCCESS, "Gradual increment poll failed");
        check(input.promptSource() == PromptInputSource::KeyboardMouse, "Small gradual drift stole prompts too early");
    }
    for (int step = 6; step <= 10; ++step) {
        inputStates[0].Gamepad.sThumbLX = SHORT(20000 + step * 1000);
        check(promptState() == ERROR_SUCCESS, "Accumulated increment poll failed");
    }
    check(input.promptSource() == PromptInputSource::Controller, "Accumulated 10000-count drift never reselected controller");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Drift reset poll failed");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    inputStates[0].Gamepad = {};
    inputStates[0].Gamepad.sThumbLX = 20000;
    check(promptState() == ERROR_SUCCESS, "Jitter anchor poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Jitter anchor did not switch prompts");
    SendMessageW(window, WM_KEYDOWN, 'W', 0);
    check(promptState() == ERROR_SUCCESS, "Jitter reset poll failed");
    SendMessageW(window, WM_KEYUP, 'W', 0);
    for (int i = 0; i < 20; ++i) {
        inputStates[0].Gamepad.sThumbLX = SHORT(20000 + (i % 2 ? 400 : -400));
        check(promptState() == ERROR_SUCCESS, "Bounded jitter poll failed");
        check(input.promptSource() == PromptInputSource::KeyboardMouse, "Bounded jitter stole prompts");
    }
    // Guest bytes unchanged by classification: gradual drift still reaches guest.
    // Final loop state is i=19 -> 20000+400; compare against the actual final
    // physical value, covering all 12 gamepad bytes independently below.
    check(PPC_LOAD_U16(output + 8) == uint16_t(inputStates[0].Gamepad.sThumbLX),
          "Analog drift did not reach guest bytes");
    check(base[output + 6] == inputStates[0].Gamepad.bLeftTrigger &&
          base[output + 7] == inputStates[0].Gamepad.bRightTrigger &&
          PPC_LOAD_U16(output + 4) == inputStates[0].Gamepad.wButtons &&
          PPC_LOAD_U16(output + 10) == uint16_t(inputStates[0].Gamepad.sThumbLY) &&
          PPC_LOAD_U16(output + 12) == uint16_t(inputStates[0].Gamepad.sThumbRX) &&
          PPC_LOAD_U16(output + 14) == uint16_t(inputStates[0].Gamepad.sThumbRY),
          "Guest gamepad bytes differ from physical state");
    // Menu cursor movement switches back without click/capture; unchanged ignores.
    inputStates[0].Gamepad = {XINPUT_GAMEPAD_A, 0, 0, 0, 0, 0, 0};
    check(promptState() == ERROR_SUCCESS, "Menu-mouse baseline poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Menu-mouse baseline did not switch prompts");
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 100));
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Menu cursor move did not win prompts back eagerly");
    check(promptState() == ERROR_SUCCESS, "Menu-mouse poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Menu cursor move did not hold keyboard/mouse");
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 100));
    inputStates[0].Gamepad = {XINPUT_GAMEPAD_A, 0, 0, 0, 0, 0, 0};
    check(promptState() == ERROR_SUCCESS, "Unchanged cursor poll failed");
    // Only guest zero is exposed: secondary guest polls never steal nor clear
    // prompts, even when that Windows host slot has a connected pad. The A button
    // is still held from the cursor poll above, so synthesize a true release
    // poll followed by a new press before expecting a controller edge.
    inputStates[0].Gamepad = {};
    check(promptState() == ERROR_SUCCESS, "Primary release-before-edge poll failed");
    inputStates[0].Gamepad = {XINPUT_GAMEPAD_A, 0, 0, 0, 0, 0, 0};
    check(promptState() == ERROR_SUCCESS, "Primary edge poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Primary edge did not switch prompts");
    inputStatus[1] = ERROR_DEVICE_NOT_CONNECTED;
    inputStates[1] = {};
    check(input.getState(*memory, 1, 0, output) == ERROR_DEVICE_NOT_CONNECTED, "Secondary disconnect poll failed");
    check(input.promptSource() == PromptInputSource::Controller, "Disconnected secondary pad cleared primary prompts");
    inputStatus[1] = ERROR_SUCCESS;
    inputStates[1].Gamepad = {};
    check(input.getState(*memory, 1, 0, output) == ERROR_DEVICE_NOT_CONNECTED && zero(output, 16),
          "Idle secondary guest exposed a host controller");
    check(input.promptSource() == PromptInputSource::Controller, "Idle secondary pad cleared primary prompts");
    SendMessageW(window, WM_KEYDOWN, 'E', 0);
    check(input.getState(*memory, 1, 0, output) == ERROR_DEVICE_NOT_CONNECTED && zero(output, 16),
          "Secondary guest became connected during keyboard input");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Secondary idle poll blocked keyboard reclaim");
    SendMessageW(window, WM_KEYUP, 'E', 0);
    inputStates[0].Gamepad = {};
    check(promptState() == ERROR_SUCCESS, "Primary release poll failed");
    inputStates[1].Gamepad = {XINPUT_GAMEPAD_B, 0, 0, 0, 0, 0, 0};
    check(input.getState(*memory, 1, 0, output) == ERROR_DEVICE_NOT_CONNECTED && zero(output, 16),
          "Secondary guest exposed an unselected host edge");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Secondary pad stole primary-only prompts");
    inputStatus[1] = ERROR_DEVICE_NOT_CONNECTED;
    inputStates[1] = {};
    inputStatus[0] = ERROR_DEVICE_NOT_CONNECTED;
    inputStates[0] = {};
    check(promptState() == ERROR_SUCCESS, "Final disconnect poll failed");
    check(input.promptSource() == PromptInputSource::KeyboardMouse, "Final disconnect left stuck controller prompts");
    inputStatus[0] = ERROR_SUCCESS;
    inputStates[0] = {};
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    check(promptState() == ERROR_SUCCESS && zero(output + 4, 12), "Slot test regain replayed input");
    puts("Native input prompts: default, idle, jitter, churn, edges, held, simultaneous, mouse, disconnect and focus passed.");
    puts("Native input prompt retrigger: negative diagonal, anchor accumulation, bounded jitter, menu cursor and single guest user passed.");

    // Capture uses raw window edges while the original menu receives a neutral
    // controller. Quick clicks and the newly assigned held key cannot replay.
    cancelKeyboardMenuCapture();
    inputStatus.fill(ERROR_DEVICE_NOT_CONNECTED); inputStates = {};
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    input.setMouseLookEnabled(false); input.setGuestMenuActive(true);
    input.setKeyboardBindings(defaultKeyboardBindings());
    SendMessageW(window, WM_XBUTTONDOWN, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), 0);
    SendMessageW(window, WM_CAPTURECHANGED, 0, 0);
    check(beginKeyboardMenu(input.keyboardBindings()) && keyboardMenuAction("darkrecomp.keyboard.bind.8.0") &&
          keyboardMenuLabel("darkrecomp.keyboard.bind.8.0").find("PRESSKEY") != std::string::npos,
          "Capture loss left a stale mouse button blocking the binding editor");
    cancelKeyboardMenuCapture(); endKeyboardMenu();
    SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, LPARAM(1) << 29);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Alt+Enter fullscreen shortcut leaked guest Start");
    SendMessageW(window, WM_SYSKEYUP, VK_RETURN, LPARAM(1) << 29);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_START,
          "Ordinary Enter stopped navigating the original menu");
    SendMessageW(window, WM_SYSKEYUP, VK_RETURN, LPARAM(1) << 29);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12),
          "Releasing ordinary Enter with Alt held retained guest Start");
    SendMessageW(window, WM_KEYDOWN, 'E', 0);
    check(beginKeyboardMenu(input.keyboardBindings()) &&
          keyboardMenuAction("darkrecomp.keyboard.bind.10.0"), "Native menu capture did not start");
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Activation key leaked during capture");
    SendMessageW(window, WM_KEYUP, 'E', 0);
    const auto beforeCaptureClick = input.menuCursor();
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0,WHEEL_DELTA), 0);
    SendMessageW(window, WM_KEYDOWN, 'T', 0);
    check(!keyboardMenuCaptureActive() && keyboardMenuLabel("darkrecomp.keyboard.bind.10.0").find('T') != std::string::npos,
          "Fresh raw key was not staged by the game menu");
    check(state() == ERROR_SUCCESS && zero(output + 4, 12) &&
          input.menuCursor().presses == beforeCaptureClick.presses,
          "Capture replayed a wheel detent or assigned key");
    SendMessageW(window, WM_KEYDOWN, 'T', LPARAM(1) << 30);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Assigned-key repeat escaped capture suppression");
    SendMessageW(window, WM_KEYUP, 'T', 0);
    const struct { UINT press, release; WPARAM parameter; unsigned key; } mouseCapture[] = {
        {WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON, VK_LBUTTON},
        {WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON, VK_RBUTTON},
        {WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON, VK_MBUTTON},
        {WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON1, XBUTTON1), VK_XBUTTON1},
        {WM_XBUTTONDOWN, WM_XBUTTONUP, MAKEWPARAM(MK_XBUTTON2, XBUTTON2), VK_XBUTTON2}
    };
    for (const auto& button : mouseCapture) {
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        check(keyboardMenuAction("darkrecomp.keyboard.bind.8.0") &&
              keyboardMenuLabel("darkrecomp.keyboard.bind.8.0").find("RELEASE") != std::string::npos,
              "Slot activation click did not wait for release");
        SendMessageW(window, WM_LBUTTONUP, 0, 0);
        const auto beforeMouseCapture = input.menuCursor();
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        SendMessageW(window, button.press, button.parameter, 0);
        KeyboardMenuSaveRequest mouseSave;
        check(!keyboardMenuCaptureActive() && keyboardMenuAction("darkrecomp.keyboard.save") &&
              takeKeyboardMenuSaveRequest(mouseSave) && mouseSave.bindings.keys[size_t(KeyboardAction::Jump)][0] == button.key,
              "Native mouse edge did not stage the selected binding");
        reportKeyboardMenuSave(mouseSave.id, false);
        check(state() == ERROR_SUCCESS && zero(output + 4, 12) &&
              input.menuCursor().presses == beforeMouseCapture.presses,
              "Assigned held mouse button or wheel replayed into the menu");
        SendMessageW(window, button.press, button.parameter, 0); state();
        check(zero(output + 4, 12) && input.menuCursor().presses == beforeMouseCapture.presses,
              "Held assigned mouse button escaped suppression");
        SendMessageW(window, button.release, button.parameter, 0);
    }
    for (const LPARAM context : {altContext, altContext | (LPARAM(1) << 24)}) {
        check(keyboardMenuAction("darkrecomp.keyboard.bind.8.0"), "Cannot start native Alt capture");
        SendMessageW(window, WM_SYSKEYDOWN, VK_MENU, context);
        KeyboardMenuSaveRequest altSave;
        check(!keyboardMenuCaptureActive() && keyboardMenuAction("darkrecomp.keyboard.save") &&
              takeKeyboardMenuSaveRequest(altSave) && altSave.bindings.keys[size_t(KeyboardAction::Jump)][0] == VK_MENU,
              "Left/right Alt system message did not stage the binding");
        reportKeyboardMenuSave(altSave.id, false); state();
        check(zero(output + 4, 12), "Captured Alt leaked its gameplay action into the menu");
        SendMessageW(window, WM_SYSKEYUP, VK_MENU, context); state();
        check(zero(output + 4, 12), "Captured Alt release leaked into the menu");
    }
    check(keyboardMenuAction("darkrecomp.keyboard.bind.11.0"), "Second capture did not start");
    inputStatus[0] = ERROR_SUCCESS; inputStates[0].Gamepad.wButtons = XINPUT_GAMEPAD_A;
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Physical controller navigated while capturing");
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!keyboardMenuCaptureActive() && state() == ERROR_SUCCESS && zero(output + 4, 12),
          "Capture Escape or held physical confirm leaked to the menu");
    SendMessageW(window, WM_KEYUP, VK_ESCAPE, 0);
    inputStates[0].Gamepad = {}; state();
    inputStates[0].Gamepad.wButtons = XINPUT_GAMEPAD_A;
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_A,
          "Controller navigation did not resume after physical release");
    inputStates[0].Gamepad = {}; state();
    keyboardMenuAction("darkrecomp.keyboard.bind.11.0");
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    check(!keyboardMenuCaptureActive() && keyboardMenuLabel("darkrecomp.keyboard.bind.10.0").find('T') != std::string::npos,
          "Focus loss retained capture or discarded staged bindings");
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    SendMessageW(window, WM_KEYDOWN, 'E', 0);
    keyboardMenuAction("darkrecomp.keyboard.cancel");
    input.suppressMenuActivationKeys();
    endKeyboardMenu();
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Held Cancel confirmation reopened Controls submenu");
    SendMessageW(window, WM_KEYDOWN, 'E', LPARAM(1) << 30);
    check(state() == ERROR_SUCCESS && zero(output + 4, 12), "Cancel autorepeat leaked after page pop");
    SendMessageW(window, WM_KEYUP, 'E', 0);
    SendMessageW(window, WM_KEYDOWN, 'E', 0);
    check(state() == ERROR_SUCCESS && PPC_LOAD_U16(output + 4) == XINPUT_GAMEPAD_A,
          "Fresh confirmation did not resume after Cancel release");
    SendMessageW(window, WM_KEYUP, 'E', 0);

    // Finally call the title's actual SDK wrapper through native Windows
    // message routing. Physical device presence does not affect these checks:
    // keyboard Start is merged, and loss of focus neutralizes both sources.
    initializeKernel();
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&nativeInput()));
    nativeInput().attachWindow(window);
    SendMessageW(window, WM_SETFOCUS, 0, 0);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    ctx.r3.u64 = 0; ctx.r4.u64 = output; ctx.r5.u64 = 0;
    sub_828AAAB8(ctx, base);
    check(ctx.r3.u32 == ERROR_SUCCESS && (PPC_LOAD_U16(output + 4) & XINPUT_GAMEPAD_START),
          "Original SDK wrapper did not consume native Start input using its r4 -> r5 ABI");
    SendMessageW(window,WM_KEYUP,VK_RETURN,0);
    nativeInput().setMouseLookEnabled(true);nativeInput().mouseMotion(32,-16);
    ctx.r3.u64=0;ctx.r4.u64=output;ctx.r5.u64=0;sub_828AAAB8(ctx,base);
    const auto sdkMouse=nativeInput().consumeMouseLook();
    check(ctx.r3.u32==ERROR_SUCCESS && sdkMouse.x==32 && sdkMouse.y==-16,
          "Original SDK wrapper consumed relative counts before gameplay");
    SendMessageW(window, WM_KILLFOCUS, 0, 0);
    ctx.r3.u64 = 0; ctx.r4.u64 = output;
    sub_828AAAB8(ctx, base);
    check(ctx.r3.u32 == ERROR_SUCCESS && zero(output + 4, 12), "Original SDK wrapper retained background input");
    ctx.r3.u64 = 0; ctx.r4.u64 = XINPUT_FLAG_GAMEPAD; ctx.r5.u64 = output;
    sub_828AAAB0(ctx, base);
    check(ctx.r3.u32 == ERROR_SUCCESS && base[output] == XINPUT_DEVTYPE_GAMEPAD,
          "Original capabilities wrapper did not find the native input device");
    memset(base + vibration, 0, 4);
    ctx.r3.u64 = 0; ctx.r4.u64 = vibration;
    sub_828AAAC8(ctx, base);
    check(ctx.r3.u32 == ERROR_SUCCESS, "Original vibration wrapper did not forward its r5 descriptor");
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    memory->release(storage);
    puts("Native input: exact guest ABI, keyboard actions, relative mouse, wheel, focus, rumble and original SDK wrappers passed.");
}
