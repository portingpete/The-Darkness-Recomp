#include "input.h"
#include "keyboard_menu.h"
#include "runtime.h"
#include "stall_profiler.h"
#include "stall_profiler_lock.h"
#include "renderer/engine/prompt_bindings.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace DarkRecomp::Native {
namespace {
unsigned mouseMessageKey(UINT message, WPARAM parameter) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return VK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return VK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        switch (GET_XBUTTON_WPARAM(parameter)) {
        case XBUTTON1: return VK_XBUTTON1;
        case XBUTTON2: return VK_XBUTTON2;
        default: return 0;
        }
    default: return 0;
    }
}
bool guestSpan(Memory& owner, uint32_t address, uint32_t length, bool writable) {
    if (!address || uint64_t(address) + length > 0x100000000ull) return false;
    uint64_t cursor = address, end = cursor + length;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        const auto* pointer = owner.base() + cursor;
        SIZE_T queried;
        {
            StallProfiler::Scope query(StallProfiler::Section::Other, "input guest-buffer VirtualQuery",
                0, 0, cursor, "guest input buffer");
            queried = VirtualQuery(pointer, &info, sizeof(info));
        }
        if (!queried || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        DWORD protection = info.Protect & 0xff;
        bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (writable ? !canWrite : !(canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ))
            return false;
        uint64_t next = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - owner.base();
        if (next <= cursor) return false;
        cursor = (std::min)(next, end);
    }
    return true;
}
void write16(uint8_t* output, uint16_t value) {
    output[0] = uint8_t(value >> 8);
    output[1] = uint8_t(value);
}
uint16_t read16(const uint8_t* input) {
    return uint16_t((uint16_t(input[0]) << 8) | input[1]);
}
void writeGamepad(uint8_t* output, const XINPUT_GAMEPAD& value) {
    write16(output, value.wButtons);
    output[2] = value.bLeftTrigger;
    output[3] = value.bRightTrigger;
    write16(output + 4, uint16_t(value.sThumbLX));
    write16(output + 6, uint16_t(value.sThumbLY));
    write16(output + 8, uint16_t(value.sThumbRX));
    write16(output + 10, uint16_t(value.sThumbRY));
}
void mergeKeyboard(XINPUT_GAMEPAD& result, const XINPUT_GAMEPAD& keyboard) {
    result.wButtons |= keyboard.wButtons;
    result.bLeftTrigger = (std::max)(result.bLeftTrigger, keyboard.bLeftTrigger);
    result.bRightTrigger = (std::max)(result.bRightTrigger, keyboard.bRightTrigger);
    if (keyboard.sThumbLX) result.sThumbLX = keyboard.sThumbLX;
    if (keyboard.sThumbLY) result.sThumbLY = keyboard.sThumbLY;
    if (keyboard.sThumbRX) result.sThumbRX = keyboard.sThumbRX;
    if (keyboard.sThumbRY) result.sThumbRY = keyboard.sThumbRY;
}
} // namespace

bool NativeInput::controllerEdgeLocked(const XINPUT_GAMEPAD& current, const XINPUT_GAMEPAD& previous, bool haveBaseline) {
    auto stickOutside = [](SHORT x, SHORT y, int deadzone) {
        const int64_t dx = x, dy = y, dead = deadzone;
        return dx * dx + dy * dy > dead * dead;
    };
    auto stickDelta2 = [](SHORT ax, SHORT ay, SHORT bx, SHORT by) {
        const int64_t dx = int64_t(ax) - int64_t(bx), dy = int64_t(ay) - int64_t(by);
        return dx * dx + dy * dy;
    };
    constexpr int64_t kStickReselect2 = int64_t(6000) * 6000;
    constexpr int kTriggerReselect = 30;
    const bool leftNow = stickOutside(current.sThumbLX, current.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    const bool rightNow = stickOutside(current.sThumbRX, current.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    const bool leftTriggerNow = current.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const bool rightTriggerNow = current.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    if (!haveBaseline)
        return current.wButtons || leftTriggerNow || rightTriggerNow || leftNow || rightNow;
    if (current.wButtons & ~previous.wButtons) return true;
    if (leftTriggerNow) {
        if (previous.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return true;
        const int delta = current.bLeftTrigger > previous.bLeftTrigger ?
            current.bLeftTrigger - previous.bLeftTrigger : previous.bLeftTrigger - current.bLeftTrigger;
        if (delta >= kTriggerReselect) return true;
    }
    if (rightTriggerNow) {
        if (previous.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) return true;
        const int delta = current.bRightTrigger > previous.bRightTrigger ?
            current.bRightTrigger - previous.bRightTrigger : previous.bRightTrigger - current.bRightTrigger;
        if (delta >= kTriggerReselect) return true;
    }
    if (leftNow) {
        if (!stickOutside(previous.sThumbLX, previous.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)) return true;
        if (stickDelta2(current.sThumbLX, current.sThumbLY, previous.sThumbLX, previous.sThumbLY) > kStickReselect2)
            return true;
    }
    if (rightNow) {
        if (!stickOutside(previous.sThumbRX, previous.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)) return true;
        if (stickDelta2(current.sThumbRX, current.sThumbRY, previous.sThumbRX, previous.sThumbRY) > kStickReselect2)
            return true;
    }
    return false;
}
void NativeInput::resetPromptLocked() {
    promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
    lastPhysical_ = {};
    physicalBaseline_.fill(false);
    for (auto& anchor : analogAnchor_) anchor.valid = false;
    selectingSlot_ = -1;
}
void NativeInput::noteKeyboardActivityLocked(bool active) {
    if (active) promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
}
void NativeInput::updatePromptSourceLocked(uint32_t user, const XINPUT_GAMEPAD& physical, bool connected, bool keyboardMouseActive) {
    // Primary-only classification: only slot 0 feeds the title's prompt
    // choice. Secondary slots never steal nor clear, so idle/disconnected
    // polls from another device cannot undo the primary decision.
    if (user >= lastPhysical_.size()) return;
    if (user != 0) {
        if (!connected) { lastPhysical_[user] = {}; physicalBaseline_[user] = false; }
        else { lastPhysical_[user] = physical; physicalBaseline_[user] = true; }
        return;
    }
    if (!connected) {
        lastPhysical_[0] = {};
        physicalBaseline_[0] = false;
        analogAnchor_[0].valid = false;
        selectingSlot_ = -1;
        promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
        return;
    }
    auto outside = [](SHORT x, SHORT y, int deadzone) {
        const int64_t dx = x, dy = y, dead = deadzone;
        return dx * dx + dy * dy > dead * dead;
    };
    constexpr int64_t kStickReselect2 = int64_t(6000) * 6000;
    constexpr int kTriggerReselect = 30;
    const bool leftNow = outside(physical.sThumbLX, physical.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    const bool rightNow = outside(physical.sThumbRX, physical.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    const bool ltNow = physical.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const bool rtNow = physical.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const bool hadBaseline = physicalBaseline_[0];
    const bool buttonEdge = hadBaseline ?
        ((physical.wButtons & ~lastPhysical_[0].wButtons) != 0) : (physical.wButtons != 0);
    bool analogEdge = false;
    auto& anchor = analogAnchor_[0];
    auto setAnchor = [&] {
        anchor.lx = physical.sThumbLX; anchor.ly = physical.sThumbLY;
        anchor.rx = physical.sThumbRX; anchor.ry = physical.sThumbRY;
        anchor.lt = physical.bLeftTrigger; anchor.rt = physical.bRightTrigger;
        anchor.valid = true;
    };
    if (!hadBaseline || !anchor.valid) {
        analogEdge = leftNow || rightNow || ltNow || rtNow;
        setAnchor();
    } else {
        const bool anchorLeftOut = outside(anchor.lx, anchor.ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        const bool anchorRightOut = outside(anchor.rx, anchor.ry, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        const bool anchorLt = anchor.lt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        const bool anchorRt = anchor.rt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        if (leftNow) {
            if (!anchorLeftOut) analogEdge = true;
            else {
                const int64_t dx = int64_t(physical.sThumbLX) - anchor.lx;
                const int64_t dy = int64_t(physical.sThumbLY) - anchor.ly;
                if (dx * dx + dy * dy > kStickReselect2) analogEdge = true;
            }
        }
        if (!analogEdge && rightNow) {
            if (!anchorRightOut) analogEdge = true;
            else {
                const int64_t dx = int64_t(physical.sThumbRX) - anchor.rx;
                const int64_t dy = int64_t(physical.sThumbRY) - anchor.ry;
                if (dx * dx + dy * dy > kStickReselect2) analogEdge = true;
            }
        }
        if (!analogEdge && ltNow) {
            if (!anchorLt) analogEdge = true;
            else {
                const int d = physical.bLeftTrigger > anchor.lt ?
                    physical.bLeftTrigger - anchor.lt : anchor.lt - physical.bLeftTrigger;
                if (d >= kTriggerReselect) analogEdge = true;
            }
        }
        if (!analogEdge && rtNow) {
            if (!anchorRt) analogEdge = true;
            else {
                const int d = physical.bRightTrigger > anchor.rt ?
                    physical.bRightTrigger - anchor.rt : anchor.rt - physical.bRightTrigger;
                if (d >= kTriggerReselect) analogEdge = true;
            }
        }
        if (analogEdge) setAnchor();
        else if (!leftNow && !rightNow && !ltNow && !rtNow) setAnchor();
    }
    const bool edge = buttonEdge || analogEdge;
    lastPhysical_[0] = physical;
    physicalBaseline_[0] = true;
    if (keyboardMouseActive) {
        promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
        selectingSlot_ = -1;
        setAnchor();
    } else if (edge) {
        promptSource_.store(PromptInputSource::Controller, std::memory_order_release);
        selectingSlot_ = 0;
        setAnchor();
    }
}

NativeInput& nativeInput() {
    static NativeInput input({}, true);
    return input;
}
NativeInput::NativeInput(ControllerApi api, bool backgroundController)
    : api_(api), backgroundController_(backgroundController) {
    if (backgroundController_) {
        try { controllerThread_ = std::thread(&NativeInput::controllerWorker, this); }
        catch (...) {
            std::fprintf(stderr, "[Input] Controller worker unavailable; physical controllers disabled, keyboard/mouse active.\n");
        }
    }
}
NativeInput::~NativeInput() {
    {
        std::lock_guard lock(mutex_);
        stopVibrationLocked();
    }
    stopControllerWorker();
}
void NativeInput::stopControllerWorker() {
    {
        std::lock_guard lock(controllerMutex_);
        controllerStop_ = true;
    }
    controllerChanged_.notify_one();
    if (controllerThread_.joinable()) controllerThread_.join();
}
DWORD NativeInput::physicalState(uint32_t user, XINPUT_STATE& state, uint64_t& generation) {
    if (!backgroundController_) { generation = UINT64_MAX; return api_.state(user, &state); }
    auto lock = StallProfiler::lock(controllerMutex_, "NativeInput::physicalState cache mutex");
    state = physicalSlots_[user].state;
    generation = physicalSlots_[user].generation;
    return physicalSlots_[user].status;
}
DWORD NativeInput::physicalCapabilities(uint32_t user, uint32_t flags, XINPUT_CAPABILITIES& caps) {
    if (!backgroundController_) return api_.capabilities(user, flags, &caps);
    auto lock = StallProfiler::lock(controllerMutex_, "NativeInput::physicalCapabilities cache mutex");
    const auto& physical = physicalSlots_[user];
    caps = physical.caps;
    // XINPUT_FLAG_GAMEPAD only filters Type; capability bytes are otherwise
    // the same snapshot obtained by the unrestricted host query.
    if (physical.capsStatus == ERROR_SUCCESS && flags == XINPUT_FLAG_GAMEPAD &&
        caps.Type != XINPUT_DEVTYPE_GAMEPAD) return ERROR_DEVICE_NOT_CONNECTED;
    return physical.capsStatus;
}
DWORD NativeInput::physicalVibration(uint32_t user, XINPUT_VIBRATION vibration) {
    if (!backgroundController_) return api_.vibration(user, &vibration);
    DWORD status;
    {
        auto lock = StallProfiler::lock(controllerMutex_, "NativeInput::physicalVibration cache mutex");
        auto& physical = physicalSlots_[user];
        status = physical.status;
        if (status != ERROR_SUCCESS && (vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed))
            return status; // Never replay a rejected request on a later hotplug.
        // Latest command wins, including a focus-loss zero replacing an
        // unsent rumble request while a device probe is blocked in Windows.
        physical.vibration = vibration;
        ++physical.vibrationVersion;
        controllerCommand_ = true;
    }
    controllerChanged_.notify_one();
    return status;
}
void NativeInput::controllerWorker() {
    using Clock = std::chrono::steady_clock;
    constexpr auto connectedInterval = std::chrono::milliseconds(2);
    constexpr auto discoveryInterval = std::chrono::milliseconds(500);
    std::array<uint64_t, XUSER_MAX_COUNT> sent{};
    std::array<XINPUT_VIBRATION, XUSER_MAX_COUNT> lastSent{};
    std::array<Clock::time_point, XUSER_MAX_COUNT> nextCaps{};
    auto nextDiscovery = Clock::time_point{};
    uint32_t discoverySlot = 0;
    const auto stopped = [&] {
        std::lock_guard lock(controllerMutex_);
        return controllerStop_;
    };
    std::array<DWORD, XUSER_MAX_COUNT> motorErrors{};
    const auto drainMotors = [&] {
        for (uint32_t user = 0; user < XUSER_MAX_COUNT && !stopped(); ++user) {
            // A newer zero that arrived inside SetState is delivered directly
            // afterward, before any other potentially blocking device probe.
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                XINPUT_VIBRATION vibration{};
                uint64_t version;
                {
                    std::lock_guard lock(controllerMutex_);
                    const auto& physical = physicalSlots_[user];
                    version = physical.vibrationVersion;
                    if (version == sent[user]) break;
                    vibration = physical.vibration;
                    if (attempt && (vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed)) break;
                }
                DWORD result;
                {
                    StallProfiler::Scope profile(StallProfiler::Section::Other, "XInputSetState(background)",
                        0, 0, user, "controller-user");
                    result = api_.vibration(user, &vibration);
                }
                if (result == ERROR_SUCCESS) lastSent[user] = vibration;
                else if (result != ERROR_DEVICE_NOT_CONNECTED && motorErrors[user] != result)
                    std::fprintf(stderr, "[Input] Background vibration user=%u status=%lu\n", user, result);
                motorErrors[user] = result;
                sent[user] = version;
            }
        }
    };
    const auto refresh = [&](uint32_t user) {
        if (stopped()) return;
        XINPUT_STATE state{};
        DWORD status;
        uint64_t generation;
        {
            std::lock_guard lock(controllerMutex_);
            generation = ++physicalSlots_[user].startedGeneration;
        }
        {
            StallProfiler::Scope profile(StallProfiler::Section::Other, "XInputGetState(background)",
                0, 0, user, "controller-user");
            status = api_.state(user, &state);
        }
        bool needsCaps;
        {
            std::lock_guard lock(controllerMutex_);
            auto& physical = physicalSlots_[user];
            const bool newlyConnected = physical.status != ERROR_SUCCESS && status == ERROR_SUCCESS;
            physical.status = status;
            physical.generation = generation;
            physical.state = status == ERROR_SUCCESS ? state : XINPUT_STATE{};
            if (status != ERROR_SUCCESS) {
                physical.caps = {};
                physical.capsStatus = status;
                // A pending command belongs to the device that accepted it,
                // never a replacement pad plugged into the same user slot.
                physical.vibration = {};
                sent[user] = physical.vibrationVersion;
            }
            needsCaps = status == ERROR_SUCCESS &&
                (newlyConnected || (physical.capsStatus != ERROR_SUCCESS && Clock::now() >= nextCaps[user]));
        }
        drainMotors();
        if (needsCaps && !stopped()) {
            XINPUT_CAPABILITIES caps{};
            DWORD capsStatus;
            {
                StallProfiler::Scope profile(StallProfiler::Section::Other, "XInputGetCapabilities(background)",
                    0, 0, user, "controller-user");
                capsStatus = api_.capabilities(user, 0, &caps);
            }
            std::lock_guard lock(controllerMutex_);
            physicalSlots_[user].caps = capsStatus == ERROR_SUCCESS ? caps : XINPUT_CAPABILITIES{};
            physicalSlots_[user].capsStatus = capsStatus;
            nextCaps[user] = Clock::now() + std::chrono::seconds(1);
        }
        drainMotors();
    };
    while (!stopped()) {
        std::array<bool, XUSER_MAX_COUNT> connected{};
        {
            std::lock_guard lock(controllerMutex_);
            controllerCommand_ = false;
            for (uint32_t user = 0; user < connected.size(); ++user)
                connected[user] = physicalSlots_[user].status == ERROR_SUCCESS;
        }
        drainMotors();
        // Refresh live pads before one absent-slot probe. Do not issue a
        // burst of four potentially slow Windows device-enumeration calls.
        for (uint32_t user = 0; user < connected.size(); ++user)
            if (connected[user]) refresh(user);
        if (Clock::now() >= nextDiscovery) {
            for (uint32_t count = 0; count < connected.size(); ++count) {
                const uint32_t user = discoverySlot++ % XUSER_MAX_COUNT;
                if (!connected[user]) { refresh(user); break; }
            }
            nextDiscovery = Clock::now() + discoveryInterval;
        }
        drainMotors();
        std::unique_lock lock(controllerMutex_);
        controllerChanged_.wait_for(lock, connectedInterval, [&] { return controllerStop_ || controllerCommand_; });
    }
    for (uint32_t user = 0; user < XUSER_MAX_COUNT; ++user)
        if (lastSent[user].wLeftMotorSpeed || lastSent[user].wRightMotorSpeed) {
            XINPUT_VIBRATION stop{};
            StallProfiler::Scope profile(StallProfiler::Section::Other, "XInputSetState(shutdown)",
                0, 0, user, "controller-user");
            api_.vibration(user, &stop);
        }
}
void NativeInput::clearKeysLocked() {
    ++menuEpoch_;
    keys_.fill(false);
    capturedKeys_.fill(false);
    heldKeys_ = 0;
    keyboardMouseEvent_ = false;
    haveMenuPos_ = false;
    escapePauses_ = false;
    mouseLook_ = false;
    clearMouseLocked();
}
void NativeInput::clearMouseLocked() {
    ++mouseEpoch_;
    for (const unsigned key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        if (keys_[key] && heldKeys_) --heldKeys_;
        keys_[key] = false;
        capturedKeys_[key] = false;
        // Capture can be lost before Windows delivers the release in this
        // window. The binding editor must not wait on that stale held button.
        keyboardMenuKeyEvent(key, false);
    }
    mouseX_ = mouseY_ = 0;
    mousePending_ = false;
    wheelPending_ = wheelRemainder_ = 0; wheelButton_ = 0; wheelNext_ = 0;
}
void NativeInput::setMouseLookEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    ++menuEpoch_;
    mouseLook_ = enabled && window_ && focused_ && !settingsOpen_;
    clearMouseLocked();
    if (keys_[VK_SPACE]) {
        keys_[VK_SPACE] = false;
        if (heldKeys_) --heldKeys_;
    }
}
bool NativeInput::mouseLookEnabled() {
    std::lock_guard lock(mutex_);
    return mouseLook_;
}
void NativeInput::setGuestMenuActive(bool active) {
    std::lock_guard lock(mutex_);
    if (guestMenuContextKnown_ && guestMenuActive_ == active) return;
    guestMenuContextKnown_ = true;
    guestMenuActive_ = active;
    ++menuEpoch_;
    // A detent belongs to the UI/gameplay context in which it arrived. Never
    // replay a dialogue choice as a weapon switch when the dialogue closes.
    wheelPending_ = wheelRemainder_ = 0;
    wheelButton_ = 0;
    wheelNext_ = 0;
}
bool NativeInput::setMouseSensitivity(float sensitivity) {
    if (!std::isfinite(sensitivity) || sensitivity < .1f || sensitivity > 10.f) return false;
    std::lock_guard lock(mutex_);
    mouseSensitivity_ = sensitivity;
    return true;
}
void NativeInput::suppressMenuActivationKeysLocked() {
    const bool held = heldKeys_ || wheelPending_ || wheelButton_;
    for (size_t key = 0; key < keys_.size(); ++key) {
        capturedKeys_[key] = capturedKeys_[key] || keys_[key];
        keys_[key] = false;
    }
    heldKeys_ = 0;
    wheelPending_ = wheelRemainder_ = 0; wheelButton_ = 0;
    waitForControllerRelease_.fill(true);
    if (backgroundController_) {
        captureWasBlocked_ = true;
        std::lock_guard physicalLock(controllerMutex_);
        for (uint32_t user = 0; user < XUSER_MAX_COUNT; ++user)
            controllerFreshAfter_[user] = physicalSlots_[user].startedGeneration + 1;
    }
    if (held) ++menuEpoch_;
}
void NativeInput::suppressMenuActivationKeys() {
    std::lock_guard lock(mutex_);
    suppressMenuActivationKeysLocked();
}
KeyboardBindings NativeInput::keyboardBindings() {
    std::lock_guard lock(mutex_);
    return bindings_;
}
bool NativeInput::setKeyboardBindings(const KeyboardBindings& bindings) {
    if (!validKeyboardBindings(bindings)) return false;
    auto labels = Prompts::defaultBindingLabels();
    const auto key = [&](KeyboardAction action) {
        const auto& slots = bindings.keys[size_t(action)];
        return keyboardKeyPrompt(slots[0] ? slots[0] : slots[1]);
    };
    const auto mouseOrKey = [&](KeyboardAction action) {
        for (const auto assigned : bindings.keys[size_t(action)])
            if (isMouseBindingKey(assigned)) return keyboardKeyPrompt(assigned);
        return key(action);
    };
    const auto alternatives = [&](KeyboardAction action) {
        std::string result;
        for (const auto assigned : bindings.keys[size_t(action)]) if (assigned) {
            if (!result.empty()) result += "/";
            result += keyboardKeyPrompt(assigned);
        }
        return result.empty() ? std::string("-") : result;
    };
    const auto withMenu = [](std::string configured, const char* menu) {
        return configured == menu ? configured : configured + "/" + menu;
    };
    labels[0] = withMenu(key(KeyboardAction::Use), "E");
    labels[1] = key(KeyboardAction::Reload) + "/Esc";
    labels[2] = key(KeyboardAction::RedirectDarkling);
    labels[3] = withMenu(key(KeyboardAction::Jump), "Spc");
    labels[4] = key(KeyboardAction::ManifestDarkness);
    labels[5] = key(KeyboardAction::UseDarkness);
    labels[6] = mouseOrKey(KeyboardAction::FireLeft);
    labels[7] = mouseOrKey(KeyboardAction::FireRight);
    labels[8] = key(KeyboardAction::Crouch);
    labels[9] = alternatives(KeyboardAction::Zoom);
    labels[10] = withMenu(key(KeyboardAction::Pause), "Ent");
    labels[11] = withMenu(key(KeyboardAction::Journal), "Tab");
    labels[12] = key(KeyboardAction::PreviousPower) + "/^";
    labels[13] = key(KeyboardAction::NextPower) + "/v";
    labels[14] = key(KeyboardAction::PreviousWeapon) + "/<";
    labels[15] = key(KeyboardAction::NextWeapon) + "/>";
    labels[16] = key(KeyboardAction::PreviousPower) + "/" + key(KeyboardAction::NextPower);
    labels[17] = key(KeyboardAction::PreviousWeapon) + "/" + key(KeyboardAction::NextWeapon);
    const std::array<std::string, 4> movement{
        key(KeyboardAction::MoveForward), key(KeyboardAction::MoveLeft),
        key(KeyboardAction::MoveBackward), key(KeyboardAction::MoveRight)};
    const bool compactMovement = std::all_of(movement.begin(), movement.end(), [](const auto& value) { return value.size() == 1; });
    labels[18] = movement[0];
    for (size_t i = 1; i < movement.size(); ++i) labels[18] += (compactMovement ? "" : "/") + movement[i];
    labels[20] = compactMovement ? labels[18] + "/Mse" : labels[18];
    labels[21] = key(KeyboardAction::Crouch) + "/" + key(KeyboardAction::Zoom);
    auto gameplayLabels = labels;
    // The original HUD owns gameplay prompts. Its B icon describes the
    // configured Reload action. Keep the fixed Back/Pause alternatives in
    // the menu/Controls label snapshot.
    gameplayLabels[0] = key(KeyboardAction::Use);
    gameplayLabels[1] = key(KeyboardAction::Reload);
    gameplayLabels[3] = key(KeyboardAction::Jump);
    gameplayLabels[10] = key(KeyboardAction::Pause);
    gameplayLabels[11] = key(KeyboardAction::Journal);
    gameplayLabels[12] = key(KeyboardAction::PreviousPower);
    gameplayLabels[13] = key(KeyboardAction::NextPower);
    gameplayLabels[14] = key(KeyboardAction::PreviousWeapon);
    gameplayLabels[15] = key(KeyboardAction::NextWeapon);
    std::lock_guard lock(mutex_);
    bindings_ = bindings;
    Prompts::setBindingLabels(std::move(labels), std::move(gameplayLabels));
    // The old held key must never keep an action active after its reassignment.
    clearKeysLocked();
    return true;
}
MenuCursorSnapshot NativeInput::menuCursor() {
    std::lock_guard lock(mutex_);
    return {lastMenuX_, lastMenuY_, window_ && focused_ && !settingsOpen_ && !keyboardMenuInputBlocked() && !mouseLook_ && haveMenuPos_,
            keys_[VK_LBUTTON], menuMovement_, menuPresses_, menuEpoch_};
}
bool NativeInput::guestMenuAllowsPointer() {
    std::lock_guard lock(mutex_);
    return !guestMenuContextKnown_ || guestMenuActive_;
}
void NativeInput::mouseMotion(LONG dx, LONG dy) {
    std::lock_guard lock(mutex_);
    if (!mouseLook_ || !focused_ || settingsOpen_ || !window_ || (!dx && !dy)) return;
    promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
    keyboardMouseEvent_ = true;
    // Keep even LONG_MIN and multiple maximum-sized packets exact. The very
    // large bound only protects an unconsumed accumulator from integer overflow.
    constexpr int64_t limit = int64_t(1) << 52;
    mouseX_ = std::clamp(mouseX_ + int64_t(dx), -limit, limit);
    mouseY_ = std::clamp(mouseY_ + int64_t(dy), -limit, limit);
    mousePending_ = true;
    ++counters_.mouseEvents;
}
MouseLookDelta NativeInput::consumeMouseLook() {
    std::lock_guard lock(mutex_);
    MouseLookDelta result{0, 0, mouseSensitivity_, mouseEpoch_};
    if (window_ && focused_ && !settingsOpen_ && mouseLook_) {
        result.x = mouseX_;
        result.y = mouseY_;
    }
    mouseX_ = mouseY_ = 0;
    mousePending_ = false;
    return result;
}
void NativeInput::stopVibrationLocked() {
    for (uint32_t user = 0; user < slots_.size(); ++user) {
        if (slots_[user].rumbling) {
            XINPUT_VIBRATION stop{};
            physicalVibration(user, stop);
            slots_[user].rumbling = false;
        }
    }
}
void NativeInput::attachWindow(HWND window) {
    std::lock_guard lock(mutex_);
    stopVibrationLocked();
    clearKeysLocked();
    resetPromptLocked();
    guestMenuActive_ = guestMenuContextKnown_ = false;
    window_ = window;
    focused_ = false;
    settingsOpen_ = false;
    captureWasBlocked_ = false;
    waitForControllerRelease_.fill(false);
}
void NativeInput::setSettingsOpen(bool open) {
    std::lock_guard lock(mutex_);
    if (settingsOpen_ == open) return;
    settingsOpen_ = open;
    clearKeysLocked();
    stopVibrationLocked();
    resetPromptLocked();
    // Save/Cancel may be pressed on any controller. Do not deliver that
    // held button to the original menu when the panel releases ownership.
    waitForControllerRelease_.fill(true);
    if (backgroundController_) {
        std::lock_guard physicalLock(controllerMutex_);
        for (uint32_t user = 0; user < XUSER_MAX_COUNT; ++user)
            controllerFreshAfter_[user] = physicalSlots_[user].startedGeneration + 1;
    }
}
bool NativeInput::windowMessage(HWND window, UINT message, WPARAM key, LPARAM detail) {
    std::lock_guard lock(mutex_);
    if (!window_ || window_ != window) return false;
    if (message == WM_ACTIVATEAPP || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        const bool wasFocused = focused_;
        focused_ = message == WM_SETFOCUS || (message == WM_ACTIVATEAPP && key != 0);
        if (!focused_) {
            cancelKeyboardMenuCapture();
            clearKeysLocked();
            stopVibrationLocked();
            resetPromptLocked();
            if (backgroundController_) {
                std::lock_guard physicalLock(controllerMutex_);
                for (uint32_t user = 0; user < XUSER_MAX_COUNT; ++user)
                    controllerFreshAfter_[user] = physicalSlots_[user].startedGeneration + 1;
            }
        } else if (!wasFocused && backgroundController_) {
            std::lock_guard physicalLock(controllerMutex_);
            for (uint32_t user = 0; user < XUSER_MAX_COUNT; ++user)
                controllerFreshAfter_[user] = physicalSlots_[user].startedGeneration + 1;
        }
    } else if (message == WM_NCDESTROY) {
        cancelKeyboardMenuCapture();
        clearKeysLocked();
        stopVibrationLocked();
        resetPromptLocked();
        guestMenuActive_ = guestMenuContextKnown_ = false;
        window_ = nullptr;
        focused_ = false;
    } else if (message == WM_CAPTURECHANGED) {
        mouseLook_ = false;
        clearMouseLocked();
        resetPromptLocked();
    } else if (focused_ && !settingsOpen_) {
        const unsigned mouseKey = mouseMessageKey(message, key);
        if (mouseKey) {
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                              message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
            // Track the slot's activation click too, so capture waits for its
            // release before assigning a new mouse button.
            const bool consumed = keyboardMenuKeyEvent(mouseKey, down);
            if (consumed || capturedKeys_[mouseKey]) {
                capturedKeys_[mouseKey] = down;
                if (keys_[mouseKey]) { keys_[mouseKey] = false; if (heldKeys_) --heldKeys_; }
                return true; // Never replay an assigned or suppressed held click.
            }
            if (mouseKey == VK_LBUTTON) {
                const int x = int(short(LOWORD(detail))), y = int(short(HIWORD(detail)));
                if (!haveMenuPos_ || x != lastMenuX_ || y != lastMenuY_) ++menuMovement_;
                lastMenuX_ = x; lastMenuY_ = y; haveMenuPos_ = true;
                if (down && !keys_[mouseKey]) ++menuPresses_;
            }
            if (down != keys_[mouseKey]) {
                keys_[mouseKey] = down;
                if (down) ++heldKeys_;
                else if (heldKeys_) --heldKeys_;
            }
            if (down) {
                keyboardMouseEvent_ = true;
                promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            }
            return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP;
        }
        if (keyboardMenuInputBlocked() && message == WM_MOUSEWHEEL) {
            wheelPending_ = wheelRemainder_ = 0; wheelButton_ = 0;
            return true;
        }
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
             message == WM_KEYUP || message == WM_SYSKEYUP) && key < keys_.size())
        {
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool consumed = keyboardMenuKeyEvent(unsigned(key), down,
                (detail & (LPARAM(1) << 30)) != 0, (detail & (LPARAM(1) << 29)) != 0);
            if (consumed || capturedKeys_[key]) {
                capturedKeys_[key] = down;
                if (keys_[key]) { keys_[key] = false; if (heldKeys_) --heldKeys_; }
                return true; // Never replay an assigned/cancelled held key.
            }
            // Host shortcuts still own their chord. If Alt is bound, retire
            // its gameplay press until release while allowing the window
            // procedure to handle fullscreen, closing and system menus.
            if (down && (detail & (LPARAM(1) << 29)) &&
                (key == VK_RETURN || key == VK_F4 || key == VK_SPACE || key == VK_TAB)) {
                capturedKeys_[VK_MENU] = capturedKeys_[VK_MENU] || keys_[VK_MENU];
                if (keys_[VK_MENU]) { keys_[VK_MENU] = false; if (heldKeys_) --heldKeys_; }
                return false;
            }
            if (down && !keys_[key] && (detail & (LPARAM(1) << 30))) return false;
            if (down != keys_[key]) {
                if (key == VK_ESCAPE && down) escapePauses_ = mouseLook_ && !guestMenuActive_;
                keys_[key] = down;
                if (down) heldKeys_ = (std::min)(heldKeys_ + 1, uint32_t(keys_.size()));
                else heldKeys_ = heldKeys_ ? heldKeys_ - 1 : 0;
            }
            if (down) {
                keyboardMouseEvent_ = true;
                promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            }
            // DefWindowProc opens the host system menu on Alt release. Bound
            // Alt belongs to gameplay, so consume both of its physical edges.
            if (key == VK_MENU && std::any_of(bindings_.keys.begin(), bindings_.keys.end(),
                [](const auto& slots) { return slots[0] == VK_MENU || slots[1] == VK_MENU; })) return true;
        }
        else if (message == WM_MOUSEWHEEL && (mouseLook_ || guestMenuActive_ || !guestMenuContextKnown_)) {
            promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(key);
            wheelPending_ = std::clamp(wheelPending_ + wheelRemainder_ / WHEEL_DELTA, -16, 16);
            wheelRemainder_ %= WHEEL_DELTA;
        }
        else if (message == WM_MOUSEMOVE) {
            const int x = int(short(LOWORD(detail))), y = int(short(HIWORD(detail)));
            if (!haveMenuPos_ || x != lastMenuX_ || y != lastMenuY_) {
                haveMenuPos_ = true;
                ++menuMovement_;
                lastMenuX_ = x; lastMenuY_ = y;
                keyboardMouseEvent_ = true;
                promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            }
        }
    }
    return false;
}
XINPUT_GAMEPAD NativeInput::keyboardLocked() {
    XINPUT_GAMEPAD pad{};
    if (!window_ || !focused_ || settingsOpen_ || keyboardMenuInputBlocked()) return pad;
    const bool menuContext = !mouseLook_ || guestMenuActive_;
    const auto fixedMenuKey = [](unsigned key) {
        return key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT ||
               key == VK_RETURN || key == VK_TAB || key == 'E' || key == VK_SPACE ||
               key == VK_BACK || key == VK_ESCAPE;
    };
    const auto down = [&](KeyboardAction action) {
        const auto& binding = bindings_.keys[size_t(action)];
        const auto pressed = [&](unsigned key) {
            return key && keys_[key] && !(menuContext && fixedMenuKey(key)) &&
                   !(isMouseBindingKey(key) && (!mouseLook_ || guestMenuActive_));
        };
        return pressed(binding[0]) || pressed(binding[1]);
    };
    const struct { KeyboardAction action; WORD button; } buttons[] = {
        {KeyboardAction::Pause, XINPUT_GAMEPAD_START}, {KeyboardAction::Journal, XINPUT_GAMEPAD_BACK},
        {KeyboardAction::Use, XINPUT_GAMEPAD_A}, {KeyboardAction::Reload, XINPUT_GAMEPAD_B},
        {KeyboardAction::RedirectDarkling, XINPUT_GAMEPAD_X},
        {KeyboardAction::ManifestDarkness, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {KeyboardAction::UseDarkness, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {KeyboardAction::Crouch, XINPUT_GAMEPAD_LEFT_THUMB}, {KeyboardAction::Zoom, XINPUT_GAMEPAD_RIGHT_THUMB},
        {KeyboardAction::PreviousWeapon, XINPUT_GAMEPAD_DPAD_LEFT},
        {KeyboardAction::NextWeapon, XINPUT_GAMEPAD_DPAD_RIGHT},
        {KeyboardAction::PreviousPower, XINPUT_GAMEPAD_DPAD_UP},
        {KeyboardAction::NextPower, XINPUT_GAMEPAD_DPAD_DOWN}
    };
    for (auto binding : buttons) if (down(binding.action)) pad.wButtons |= binding.button;
    // Fixed menu navigation survives even a completely unbound gameplay set.
    // Arrows can be assigned to movement without switching weapons in play.
    if (menuContext) {
        const struct { unsigned key; WORD button; } menu[] = {
            {VK_UP, XINPUT_GAMEPAD_DPAD_UP}, {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
            {VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
            {VK_RETURN, XINPUT_GAMEPAD_START}, {VK_TAB, XINPUT_GAMEPAD_BACK},
            {'E', XINPUT_GAMEPAD_A}, {VK_SPACE, XINPUT_GAMEPAD_A}, {VK_BACK, XINPUT_GAMEPAD_B}};
        for (auto binding : menu) if (keys_[binding.key]) pad.wButtons |= binding.button;
    } else if (down(KeyboardAction::Jump)) pad.wButtons |= XINPUT_GAMEPAD_Y;
    if (keys_[VK_ESCAPE]) pad.wButtons |= escapePauses_ ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_B;
    auto axis = [&](KeyboardAction negative, KeyboardAction positive) -> SHORT {
        if (down(negative) == down(positive)) return 0;
        return down(negative) ? -32768 : 32767;
    };
    pad.sThumbLX = axis(KeyboardAction::MoveLeft, KeyboardAction::MoveRight);
    pad.sThumbLY = axis(KeyboardAction::MoveBackward, KeyboardAction::MoveForward);
    pad.sThumbRX = axis(KeyboardAction::LookLeft, KeyboardAction::LookRight);
    pad.sThumbRY = axis(KeyboardAction::LookDown, KeyboardAction::LookUp);
    pad.bLeftTrigger = down(KeyboardAction::FireLeft) ? 255 : 0;
    pad.bRightTrigger = down(KeyboardAction::FireRight) ? 255 : 0;
    const auto now = api_.ticks();
    if (now >= wheelNext_) {
        if (wheelButton_) { wheelButton_ = 0; wheelNext_ = now + 20; }
        else if (wheelPending_) {
            const bool menu = guestMenuActive_ || (!mouseLook_ && !guestMenuContextKnown_);
            wheelButton_ = wheelPending_ > 0
                ? (menu ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_RIGHT)
                : (menu ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_LEFT);
            wheelPending_ += wheelPending_ > 0 ? -1 : 1;
            wheelNext_ = now + 40;
        }
    }
    pad.wButtons |= wheelButton_;
    return pad;
}
DWORD NativeInput::getState(Memory& owner, uint32_t user, uint32_t flags, uint32_t output) {
    if (user >= slots_.size() || flags != 0 || !guestSpan(owner, output, 16, true))
        return ERROR_INVALID_PARAMETER;
    auto lock = StallProfiler::lock(mutex_, "NativeInput::getState input mutex");
    ++counters_.polls;
    const bool capturingKey = keyboardMenuInputBlocked();
    if (capturingKey) suppressMenuActivationKeysLocked();
    else if (captureWasBlocked_) {
        captureWasBlocked_ = false;
        // A neutral poll started during capture cannot release suppression
        // afterward. Require a poll started after this ownership transition.
        std::lock_guard physicalLock(controllerMutex_);
        for (uint32_t slot = 0; slot < XUSER_MAX_COUNT; ++slot)
            controllerFreshAfter_[slot] = physicalSlots_[slot].startedGeneration + 1;
    }
    XINPUT_STATE state{};
    uint64_t physicalGeneration = 0;
    DWORD status = physicalState(user, state, physicalGeneration);
    // Focus requires a poll started after the ownership change, rather than
    // replaying a cached/in-flight sample. A fresh held button still reaches
    // the game as it did with synchronous XInput, including a newly hotplugged
    // pad. Neutral-release suppression remains specific to menus/settings.
    if (physicalGeneration < controllerFreshAfter_[user]) state.Gamepad = {};
    bool keyboard = user == 0 && window_;
    auto& slot = slots_[user];
    if (status != ERROR_SUCCESS && !(status == ERROR_DEVICE_NOT_CONNECTED && keyboard)) {
        memset(owner.base() + output, 0, 16);
        if (slot.connected) fprintf(stderr, "[Input] user=%u disconnected status=%lu\n", user, status);
        slot.connected = false;
        updatePromptSourceLocked(user, XINPUT_GAMEPAD{}, false, false);
        return status;
    }
    if (status != ERROR_SUCCESS) state = {};
    if (waitForControllerRelease_[user]) {
        const auto& pad = state.Gamepad;
        const bool neutral = !pad.wButtons &&
            pad.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD && pad.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD &&
            std::abs(int(pad.sThumbLX)) <= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
            std::abs(int(pad.sThumbLY)) <= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
            std::abs(int(pad.sThumbRX)) <= XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE &&
            std::abs(int(pad.sThumbRY)) <= XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE;
        if (!settingsOpen_ && !capturingKey && (!window_ || focused_) && neutral &&
            physicalGeneration >= controllerFreshAfter_[user]) waitForControllerRelease_[user] = false;
        state.Gamepad = {};
    }
    // Classify the raw physical pad before keyboard merge: merged guest bits
    // must never read back as controller activity (virtual keyboard safety).
    // Cached held-key count keeps this O(1); no 256-key scan per guest poll.
    if (settingsOpen_ || (window_ && !focused_)) {
        resetPromptLocked();
    } else {
        bool keyboardMouseActive = false;
        if (user == 0 && window_) {
            keyboardMouseActive = heldKeys_ != 0 || keyboardMouseEvent_ || mousePending_ || wheelPending_ != 0 ||
                wheelButton_ != 0 || wheelRemainder_ != 0;
        }
        updatePromptSourceLocked(user, state.Gamepad, status == ERROR_SUCCESS, keyboardMouseActive);
        if (user == 0) keyboardMouseEvent_ = false;
    }
    if (keyboard) mergeKeyboard(state.Gamepad, keyboardLocked());
    // Windows focus suppresses all input, including physical controllers.
    // Caps still report the attached device; returning neutral avoids inventing
    // a disconnect whenever the user switches to another application.
    if (settingsOpen_ || capturingKey || (window_ && !focused_)) state.Gamepad = {};
    bool changed = !slot.connected || memcmp(&slot.previous, &state.Gamepad, sizeof(state.Gamepad)) != 0;
    if (changed) { ++slot.packet; ++counters_.changes; }
    if (!slot.connected)
        fprintf(stderr, "[Input] user=%u connected source=%s\n", user,
                status == ERROR_SUCCESS ? (keyboard ? "XInput+keyboard" : "XInput") : "keyboard");
    slot.connected = true;
    slot.previous = state.Gamepad;
    ++counters_.connected;
    const XINPUT_GAMEPAD neutral{};
    if (memcmp(&neutral, &state.Gamepad, sizeof(neutral)) != 0) ++counters_.nonneutral;
    owner.write32(output, slot.packet);
    writeGamepad(owner.base() + output + 4, state.Gamepad);
    return ERROR_SUCCESS;
}
DWORD NativeInput::getCapabilities(Memory& owner, uint32_t user, uint32_t flags, uint32_t output) {
    if (user >= slots_.size() || (flags & ~XINPUT_FLAG_GAMEPAD) || !guestSpan(owner, output, 20, true))
        return ERROR_INVALID_PARAMETER;
    auto lock = StallProfiler::lock(mutex_, "NativeInput::getCapabilities input mutex");
    XINPUT_CAPABILITIES caps{};
    DWORD status = physicalCapabilities(user, flags, caps);
    bool keyboard = user == 0 && window_;
    if (status != ERROR_SUCCESS && !(status == ERROR_DEVICE_NOT_CONNECTED && keyboard)) {
        memset(owner.base() + output, 0, 20);
        return status;
    }
    if (status != ERROR_SUCCESS) {
        caps = {};
        caps.Type = XINPUT_DEVTYPE_GAMEPAD;
        caps.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    }
    if (keyboard) {
        caps.Gamepad.wButtons |= 0xf3ff;
        caps.Gamepad.bLeftTrigger = caps.Gamepad.bRightTrigger = 255;
        caps.Gamepad.sThumbLX = caps.Gamepad.sThumbLY = caps.Gamepad.sThumbRX = caps.Gamepad.sThumbRY = -1;
    }
    auto* out = owner.base() + output;
    out[0] = caps.Type; out[1] = caps.SubType;
    write16(out + 2, caps.Flags);
    writeGamepad(out + 4, caps.Gamepad);
    write16(out + 16, caps.Vibration.wLeftMotorSpeed);
    write16(out + 18, caps.Vibration.wRightMotorSpeed);
    return ERROR_SUCCESS;
}
DWORD NativeInput::setState(Memory& owner, uint32_t user, uint32_t flags, uint32_t input) {
    if (user >= slots_.size() || flags != 0 || !guestSpan(owner, input, 4, false))
        return ERROR_INVALID_PARAMETER;
    auto lock = StallProfiler::lock(mutex_, "NativeInput::setState input mutex");
    XINPUT_VIBRATION vibration{read16(owner.base() + input), read16(owner.base() + input + 2)};
    bool requested = vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed;
    if (settingsOpen_ || (window_ && !focused_)) vibration = {};
    DWORD status = physicalVibration(user, vibration);
    slots_[user].rumbling = status == ERROR_SUCCESS && (vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed);
    if (status == ERROR_DEVICE_NOT_CONNECTED && user == 0 && window_)
        return requested ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
    return status;
}
InputCounters NativeInput::counters() {
    std::lock_guard lock(mutex_);
    return counters_;
}
}
