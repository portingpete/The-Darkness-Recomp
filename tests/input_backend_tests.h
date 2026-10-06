#pragma once
#include <future>

namespace {
struct BackgroundInputFixture {
    std::array<std::atomic<DWORD>, XUSER_MAX_COUNT> status{}, capsStatus{};
    std::array<std::atomic<WORD>, XUSER_MAX_COUNT> buttons{};
    std::array<std::atomic<unsigned>, XUSER_MAX_COUNT> stateCompletions{};
    std::atomic<bool> blockState{true}, blockCaps{true}, nonzeroVibration{false};
    std::atomic<bool> blockVibration{false}, zeroBeforeProbe{false};
    std::atomic<DWORD> vibrationStatus{ERROR_SUCCESS};
    std::atomic<unsigned> zeroAttempts{0}, successfulStops{0}, motorProbeCount{0};
    std::atomic<unsigned> vibrationCalls{0}, stateCalls{0}, capsCalls{0};
    HANDLE stateEntered = nullptr, stateRelease = nullptr, capsEntered = nullptr, capsRelease = nullptr;
    HANDLE vibrationEntered = nullptr, vibrationRelease = nullptr;
} backgroundInput;
DWORD WINAPI backgroundInputState(DWORD user, XINPUT_STATE* state) noexcept {
    ++backgroundInput.stateCalls;
    // Capture before blocking, as a driver may do before the focus changes.
    *state = {};
    state->Gamepad.wButtons = backgroundInput.buttons[user].load();
    const DWORD status = backgroundInput.status[user].load();
    if (user == 0 && backgroundInput.blockState.load()) {
        SetEvent(backgroundInput.stateEntered);
        WaitForSingleObject(backgroundInput.stateRelease, INFINITE);
    }
    ++backgroundInput.stateCompletions[user];
    return status;
}
DWORD WINAPI backgroundInputCaps(DWORD user, DWORD, XINPUT_CAPABILITIES* caps) noexcept {
    ++backgroundInput.capsCalls;
    if (user == 0 && backgroundInput.blockCaps.load()) {
        SetEvent(backgroundInput.capsEntered);
        WaitForSingleObject(backgroundInput.capsRelease, INFINITE);
    }
    *caps = {XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD, XINPUT_CAPS_FFB_SUPPORTED,
        {0xf3ff, 255, 255, -1, -1, -1, -1}, {0x1234, 0xabcd}};
    return backgroundInput.capsStatus[user].load();
}
DWORD WINAPI backgroundInputVibration(DWORD user, XINPUT_VIBRATION* vibration) noexcept {
    const DWORD result = backgroundInput.vibrationStatus.load();
    const bool nonzero = vibration->wLeftMotorSpeed || vibration->wRightMotorSpeed;
    if (nonzero && backgroundInput.blockVibration.load()) {
        backgroundInput.motorProbeCount = backgroundInput.stateCalls.load();
        SetEvent(backgroundInput.vibrationEntered);
        WaitForSingleObject(backgroundInput.vibrationRelease, INFINITE);
    }
    ++backgroundInput.vibrationCalls;
    if (nonzero && result == ERROR_SUCCESS) backgroundInput.nonzeroVibration.store(true);
    if (!nonzero) {
        ++backgroundInput.zeroAttempts;
        if (backgroundInput.blockVibration.load())
            backgroundInput.zeroBeforeProbe = backgroundInput.stateCalls.load() == backgroundInput.motorProbeCount.load();
        if (result == ERROR_SUCCESS) ++backgroundInput.successfulStops;
    }
    return result;
}
}

static void testBackgroundInputContract() {
    for (uint32_t user = 0; user < XUSER_MAX_COUNT; ++user) {
        backgroundInput.status[user] = ERROR_DEVICE_NOT_CONNECTED;
        backgroundInput.capsStatus[user] = ERROR_SUCCESS;
        backgroundInput.buttons[user] = 0;
        backgroundInput.stateCompletions[user] = 0;
    }
    backgroundInput.status[0] = ERROR_SUCCESS;
    backgroundInput.capsStatus[0] = ERROR_GEN_FAILURE;
    backgroundInput.blockState = true; backgroundInput.blockCaps = true;
    backgroundInput.nonzeroVibration = false; backgroundInput.vibrationCalls = 0;
    backgroundInput.blockVibration = false; backgroundInput.vibrationStatus = ERROR_SUCCESS;
    backgroundInput.zeroAttempts = 0; backgroundInput.successfulStops = 0;
    backgroundInput.zeroBeforeProbe = false;
    backgroundInput.stateCalls = 0; backgroundInput.capsCalls = 0;
    for (auto* handle : {&backgroundInput.stateEntered, &backgroundInput.stateRelease,
                        &backgroundInput.capsEntered, &backgroundInput.capsRelease,
                        &backgroundInput.vibrationEntered, &backgroundInput.vibrationRelease})
        // Auto-reset permits releasing exactly one blocked sample and gating
        // the next poll, without relying on scheduler timing between resets.
        *handle = CreateEventW(nullptr, handle != &backgroundInput.stateRelease, FALSE, nullptr);
    const uint32_t fixture = memory->allocate(4096), stateOut = fixture, capsOut = fixture + 32, rumble = fixture + 64;
    auto* base = memory->base();
    HWND window = nullptr;
    std::unique_ptr<NativeInput> input;
    WNDCLASSW wc{}; wc.lpfnWndProc = inputTestWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DarkRecompBackgroundInputContract";
    const auto cleanup = [&] {
        backgroundInput.blockState = false; backgroundInput.blockCaps = false;
        backgroundInput.blockVibration = false;
        SetEvent(backgroundInput.stateRelease); SetEvent(backgroundInput.capsRelease);
        SetEvent(backgroundInput.vibrationRelease);
        cancelKeyboardMenuCapture(); endKeyboardMenu();
        if (window) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        input.reset();
        if (window) DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        for (HANDLE handle : {backgroundInput.stateEntered, backgroundInput.stateRelease,
                              backgroundInput.capsEntered, backgroundInput.capsRelease,
                              backgroundInput.vibrationEntered, backgroundInput.vibrationRelease}) if (handle) CloseHandle(handle);
        if (fixture) memory->release(fixture);
    };
    const auto until = [&](auto condition) {
        const auto deadline = GetTickCount64() + 5000;
        do { if (condition()) return true; Sleep(1); } while (GetTickCount64() < deadline);
        return false;
    };
    try {
        check(fixture && backgroundInput.stateEntered && backgroundInput.stateRelease &&
              backgroundInput.capsEntered && backgroundInput.capsRelease, "Background input fixture allocation failed");
        check(backgroundInput.vibrationEntered && backgroundInput.vibrationRelease,
              "Background motor fixture allocation failed");
        check(RegisterClassW(&wc) != 0, "Cannot register background input window");
        window = CreateWindowExW(0, wc.lpszClassName, L"Background input contract", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        check(window != nullptr, "Cannot create background input window");
        input = std::make_unique<NativeInput>(ControllerApi{backgroundInputState, backgroundInputCaps,
            backgroundInputVibration, GetTickCount64}, true);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(input.get()));
        input->attachWindow(window);
        SendMessageW(window, WM_SETFOCUS, 0, 0);
        check(WaitForSingleObject(backgroundInput.stateEntered, 2000) == WAIT_OBJECT_0,
              "Background state probe did not start");
        // A stuck driver must not prevent window messages, keyboard changes,
        // guest reads or counters. Release the gates before failing the test.
        auto query = std::async(std::launch::async, [&] {
            input->windowMessage(window, WM_KILLFOCUS, 0, 0);
            input->windowMessage(window, WM_SETFOCUS, 0, 0);
            input->windowMessage(window, WM_KEYDOWN, VK_RETURN, 0);
            return input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
                PPC_LOAD_U16(stateOut + 4) == XINPUT_GAMEPAD_START &&
                input->getCapabilities(*memory, 0, XINPUT_FLAG_GAMEPAD, capsOut) == ERROR_SUCCESS &&
                memory->base()[capsOut] == XINPUT_DEVTYPE_GAMEPAD && input->counters().polls == 1;
        });
        const bool ready = query.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
        if (!ready) { SetEvent(backgroundInput.stateRelease); SetEvent(backgroundInput.capsRelease); }
        check(query.get() && ready, "Blocked OS state probe stalled keyboard, capabilities, focus or counters");
        backgroundInput.blockState = false; SetEvent(backgroundInput.stateRelease);
        check(WaitForSingleObject(backgroundInput.capsEntered, 2000) == WAIT_OBJECT_0,
              "Background capabilities probe did not start");
        query = std::async(std::launch::async, [&] {
            input->windowMessage(window, WM_KEYUP, VK_RETURN, 0);
            input->windowMessage(window, WM_KEYDOWN, 'E', 0);
            return input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
                PPC_LOAD_U16(stateOut + 4) == XINPUT_GAMEPAD_A &&
                input->getCapabilities(*memory, 0, 0, capsOut) == ERROR_SUCCESS;
        });
        const bool capsReady = query.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
        if (!capsReady) SetEvent(backgroundInput.capsRelease);
        check(query.get() && capsReady, "Blocked capabilities enumeration stalled current keyboard input");
        backgroundInput.blockCaps = false; SetEvent(backgroundInput.capsRelease);
        check(until([&] { return input->getCapabilities(*memory, 0, 0, capsOut) == ERROR_GEN_FAILURE; }),
              "Asynchronous capabilities error was hidden");
        backgroundInput.capsStatus[0] = ERROR_SUCCESS;
        check(until([&] { return input->getCapabilities(*memory, 0, 0, capsOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(capsOut + 16) == 0x1234; }), "Failed capabilities probe never refreshed");
        input->windowMessage(window, WM_KEYUP, 'E', 0);
        ResetEvent(backgroundInput.stateEntered); ResetEvent(backgroundInput.stateRelease);
        backgroundInput.blockState = true;
        check(WaitForSingleObject(backgroundInput.stateEntered, 2000) == WAIT_OBJECT_0,
              "Second blocked state probe did not start");
        PPC_STORE_U16(rumble, 0x1234); PPC_STORE_U16(rumble + 2, 0xabcd);
        check(input->setState(*memory, 0, 0, rumble) == ERROR_SUCCESS, "Connected pad rejected queued rumble");
        input->windowMessage(window, WM_KILLFOCUS, 0, 0);
        input->windowMessage(window, WM_SETFOCUS, 0, 0);
        check(input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS && PPC_LOAD_U16(stateOut + 4) == 0,
              "Focus regain replayed a stale controller snapshot");
        backgroundInput.buttons[0] = XINPUT_GAMEPAD_B;
        backgroundInput.blockState = false; SetEvent(backgroundInput.stateRelease);
        check(until([&] { return backgroundInput.vibrationCalls.load() != 0; }) &&
              !backgroundInput.nonzeroVibration.load(), "Focus-loss stop replayed an unsent nonzero rumble");
        check(until([&] { return input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(stateOut + 4) == XINPUT_GAMEPAD_B; }),
              "Focus regain suppressed a freshly polled held controller button");
        backgroundInput.buttons[0] = 0;
        const auto neutralBegin = backgroundInput.stateCompletions[0].load();
        check(until([&] { return backgroundInput.stateCompletions[0].load() >= neutralBegin + 3 &&
            input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS && PPC_LOAD_U16(stateOut + 4) == 0; }),
              "Controller release was not observed");
        backgroundInput.buttons[0] = XINPUT_GAMEPAD_Y;
        check(until([&] { return input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(stateOut + 4) == XINPUT_GAMEPAD_Y; }), "Fresh physical input did not resume after release");
        backgroundInput.buttons[0] = 0;

        // Check ordinary hotplug before capture deliberately arms release
        // suppression for every slot. The two contracts need independent
        // preconditions: a new held pad is accepted outside capture, while a
        // pad held through capture must first produce a neutral sample.
        backgroundInput.status[1] = ERROR_SUCCESS; backgroundInput.buttons[1] = XINPUT_GAMEPAD_X;
        check(until([&] { return input->getState(*memory, 1, 0, stateOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(stateOut + 4) == XINPUT_GAMEPAD_X &&
            input->getCapabilities(*memory, 1, XINPUT_FLAG_GAMEPAD, capsOut) == ERROR_SUCCESS; }),
              "Late physical-controller hotplug was not discovered");
        backgroundInput.status[1] = ERROR_DEVICE_NOT_CONNECTED;
        check(until([&] { return input->getState(*memory, 1, 0, stateOut) == ERROR_DEVICE_NOT_CONNECTED &&
            input->getCapabilities(*memory, 1, 0, capsOut) == ERROR_DEVICE_NOT_CONNECTED &&
            memory->read32(stateOut + 4) == 0 && memory->read32(capsOut) == 0; }),
              "Controller disconnect retained stale state or capabilities");

        // Complete one old neutral poll during capture, then hold the next
        // neutral poll in flight across capture close. It must not clear the
        // physical-release requirement for a button pressed during capture.
        check(until([&] { return input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(stateOut + 4) == 0; }), "Cannot establish neutral capture baseline");
        ResetEvent(backgroundInput.stateEntered); ResetEvent(backgroundInput.stateRelease);
        backgroundInput.blockState = true;
        check(WaitForSingleObject(backgroundInput.stateEntered, 2000) == WAIT_OBJECT_0,
              "Pre-capture state probe did not block");
        check(beginKeyboardMenu(input->keyboardBindings()) &&
              keyboardMenuAction("darkrecomp.keyboard.bind.10.0"), "Background binding capture did not start");
        input->suppressMenuActivationKeys();
        check(input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS && PPC_LOAD_U16(stateOut + 4) == 0,
              "Binding capture did not suppress the physical controller");
        ResetEvent(backgroundInput.stateEntered);
        SetEvent(backgroundInput.stateRelease);
        check(WaitForSingleObject(backgroundInput.stateEntered, 2000) == WAIT_OBJECT_0,
              "In-capture neutral state probe did not block");
        backgroundInput.buttons[0] = XINPUT_GAMEPAD_B;
        input->windowMessage(window, WM_KEYDOWN, VK_ESCAPE, 0);
        input->windowMessage(window, WM_KEYUP, VK_ESCAPE, 0);
        check(!keyboardMenuInputBlocked() && input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
              PPC_LOAD_U16(stateOut + 4) == 0, "Capture close replayed a cached controller sample");
        ResetEvent(backgroundInput.stateEntered);
        SetEvent(backgroundInput.stateRelease);
        check(WaitForSingleObject(backgroundInput.stateEntered, 2000) == WAIT_OBJECT_0,
              "Fresh held-button state probe did not block");
        check(input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS && PPC_LOAD_U16(stateOut + 4) == 0,
              "Old in-capture neutral sample released physical suppression");
        const auto heldBegin = backgroundInput.stateCompletions[0].load();
        backgroundInput.blockState = false;
        SetEvent(backgroundInput.stateRelease);
        check(until([&] { return backgroundInput.stateCompletions[0].load() >= heldBegin + 3 &&
            input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS && PPC_LOAD_U16(stateOut + 4) == 0; }),
              "Button held through capture leaked after an old neutral sample");
        backgroundInput.buttons[0] = 0;
        const auto captureRelease = backgroundInput.stateCompletions[0].load();
        check(until([&] { return backgroundInput.stateCompletions[0].load() >= captureRelease + 3 &&
            input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS && PPC_LOAD_U16(stateOut + 4) == 0; }),
              "Fresh controller release after capture was not observed");
        backgroundInput.buttons[0] = XINPUT_GAMEPAD_Y;
        check(until([&] { return input->getState(*memory, 0, 0, stateOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(stateOut + 4) == XINPUT_GAMEPAD_Y; }),
              "Fresh controller press after capture release did not resume");
        endKeyboardMenu();
        backgroundInput.buttons[0] = 0;
        // Reject a request while discovery is blocked before a new device is
        // visible; that old command must never play on the replacement pad.
        backgroundInput.status[0] = ERROR_DEVICE_NOT_CONNECTED;
        check(until([&] { return input->getCapabilities(*memory, 0, 0, capsOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(capsOut + 16) == 0; }), "Primary disconnect did not restore virtual keyboard capabilities");
        check(input->setState(*memory, 0, 0, rumble) == ERROR_NOT_SUPPORTED, "Absent pad accepted nonzero vibration");
        backgroundInput.status[0] = ERROR_SUCCESS;
        check(until([&] { return input->getCapabilities(*memory, 0, 0, capsOut) == ERROR_SUCCESS &&
            PPC_LOAD_U16(capsOut + 16) == 0x1234; }) && !backgroundInput.nonzeroVibration.load(),
              "A rejected vibration request replayed on hotplug");
        backgroundInput.blockVibration = true;
        check(input->setState(*memory, 0, 0, rumble) == ERROR_SUCCESS, "Live controller motor command rejected");
        check(WaitForSingleObject(backgroundInput.vibrationEntered, 2000) == WAIT_OBJECT_0,
              "Background vibration call did not start");
        const auto zeroBegin = backgroundInput.zeroAttempts.load();
        const auto successfulStops = backgroundInput.successfulStops.load();
        backgroundInput.vibrationStatus = ERROR_GEN_FAILURE;
        input->windowMessage(window, WM_KILLFOCUS, 0, 0);
        SetEvent(backgroundInput.vibrationRelease);
        check(until([&] { return backgroundInput.zeroAttempts.load() > zeroBegin; }) &&
              backgroundInput.zeroBeforeProbe.load(), "Focus-loss zero was delayed behind a fresh device probe");
        backgroundInput.blockVibration = false;
        backgroundInput.vibrationStatus = ERROR_SUCCESS;
        ResetEvent(backgroundInput.stateEntered); ResetEvent(backgroundInput.stateRelease);
        backgroundInput.blockState = true;
        check(WaitForSingleObject(backgroundInput.stateEntered, 2000) == WAIT_OBJECT_0,
              "Shutdown probe did not block");
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        auto shutdown = std::async(std::launch::async, [&] { input.reset(); });
        const bool joinedEarly = shutdown.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready;
        SetEvent(backgroundInput.stateRelease); shutdown.get();
        check(!joinedEarly, "Input backend destructor detached a blocked controller worker");
        check(backgroundInput.successfulStops.load() > successfulStops,
              "Shutdown forgot a successfully started motor after a failed zero command");
    } catch (...) { cleanup(); throw; }
    cleanup();
    puts("Background input: blocked state/caps, keyboard/focus, capability errors, hotplug, disconnect, coalesced rumble and worker join passed.");
}
