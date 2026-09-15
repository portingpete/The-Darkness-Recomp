#include "input.h"
#include "runtime.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace DarkRecomp::Native {
namespace {
bool guestSpan(Memory& owner, uint32_t address, uint32_t length, bool writable) {
    if (!address || uint64_t(address) + length > 0x100000000ull) return false;
    uint64_t cursor = address, end = cursor + length;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        const auto* pointer = owner.base() + cursor;
        if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT ||
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
    static NativeInput input;
    return input;
}
NativeInput::~NativeInput() {
    std::lock_guard lock(mutex_);
    stopVibrationLocked();
}
void NativeInput::clearKeysLocked() {
    keys_.fill(false);
    heldKeys_ = 0;
    keyboardMouseEvent_ = false;
    haveMenuPos_ = false;
    escapePauses_ = false;
    mouseLook_ = false;
    clearMouseLocked();
}
void NativeInput::clearMouseLocked() {
    ++mouseEpoch_;
    leftMouse_ = rightMouse_ = middleMouse_ = false;
    mouseX_ = mouseY_ = 0;
    mousePending_ = false;
    wheelPending_ = wheelRemainder_ = 0; wheelButton_ = 0; wheelNext_ = 0;
}
void NativeInput::setMouseLookEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
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
bool NativeInput::setMouseSensitivity(float sensitivity) {
    if (!std::isfinite(sensitivity) || sensitivity < .1f || sensitivity > 10.f) return false;
    std::lock_guard lock(mutex_);
    mouseSensitivity_ = sensitivity;
    return true;
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
            api_.vibration(user, &stop);
            slots_[user].rumbling = false;
        }
    }
}
void NativeInput::attachWindow(HWND window) {
    std::lock_guard lock(mutex_);
    stopVibrationLocked();
    clearKeysLocked();
    resetPromptLocked();
    window_ = window;
    focused_ = false;
    settingsOpen_ = false;
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
}
void NativeInput::windowMessage(HWND window, UINT message, WPARAM key, LPARAM detail) {
    std::lock_guard lock(mutex_);
    if (!window_ || window_ != window) return;
    if (message == WM_ACTIVATEAPP || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        focused_ = message == WM_SETFOCUS || (message == WM_ACTIVATEAPP && key != 0);
        if (!focused_) {
            clearKeysLocked();
            stopVibrationLocked();
            resetPromptLocked();
        }
    } else if (message == WM_NCDESTROY) {
        clearKeysLocked();
        stopVibrationLocked();
        resetPromptLocked();
        window_ = nullptr;
        focused_ = false;
    } else if (message == WM_CAPTURECHANGED) {
        mouseLook_ = false;
        clearMouseLocked();
        resetPromptLocked();
    } else if (focused_ && !settingsOpen_) {
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
             message == WM_KEYUP || message == WM_SYSKEYUP) && key < keys_.size())
        {
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            if (down && !keys_[key] && (detail & (LPARAM(1) << 30))) return;
            if (down != keys_[key]) {
                if (key == VK_ESCAPE && down) escapePauses_ = mouseLook_;
                keys_[key] = down;
                if (down) heldKeys_ = (std::min)(heldKeys_ + 1, uint32_t(keys_.size()));
                else heldKeys_ = heldKeys_ ? heldKeys_ - 1 : 0;
            }
            if (down) {
                keyboardMouseEvent_ = true;
                promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            }
        }
        else if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
            leftMouse_ = message == WM_LBUTTONDOWN;
            if (leftMouse_) promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
        }
        else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) {
            rightMouse_ = message == WM_RBUTTONDOWN;
            if (rightMouse_) promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
        }
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) {
            middleMouse_ = message == WM_MBUTTONDOWN;
            if (middleMouse_) promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
        }
        else if (message == WM_MOUSEWHEEL && mouseLook_) {
            promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(key);
            wheelPending_ = std::clamp(wheelPending_ + wheelRemainder_ / WHEEL_DELTA, -16, 16);
            wheelRemainder_ %= WHEEL_DELTA;
        }
        else if (message == WM_MOUSEMOVE) {
            const int x = int(short(LOWORD(detail))), y = int(short(HIWORD(detail)));
            if (!haveMenuPos_ || x != lastMenuX_ || y != lastMenuY_) {
                haveMenuPos_ = true;
                lastMenuX_ = x; lastMenuY_ = y;
                keyboardMouseEvent_ = true;
                promptSource_.store(PromptInputSource::KeyboardMouse, std::memory_order_release);
            }
        }
    }
}
XINPUT_GAMEPAD NativeInput::keyboardLocked() {
    XINPUT_GAMEPAD pad{};
    if (!window_ || !focused_ || settingsOpen_) return pad;
    const struct { unsigned key; WORD button; } buttons[] = {
        {VK_UP, XINPUT_GAMEPAD_DPAD_UP}, {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        {VK_RETURN, XINPUT_GAMEPAD_START}, {VK_TAB, XINPUT_GAMEPAD_BACK},
        {'E', XINPUT_GAMEPAD_A}, {'R', XINPUT_GAMEPAD_B}, {VK_BACK, XINPUT_GAMEPAD_B},
        {'F', XINPUT_GAMEPAD_X},
        {'Q', XINPUT_GAMEPAD_LEFT_SHOULDER}, {'G', XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {VK_CONTROL, XINPUT_GAMEPAD_LEFT_THUMB}, {'C', XINPUT_GAMEPAD_LEFT_THUMB},
        {VK_SHIFT, XINPUT_GAMEPAD_RIGHT_THUMB},
        {'1', XINPUT_GAMEPAD_DPAD_LEFT}, {'2', XINPUT_GAMEPAD_DPAD_RIGHT},
        {'3', XINPUT_GAMEPAD_DPAD_UP}, {'4', XINPUT_GAMEPAD_DPAD_DOWN}
    };
    for (auto binding : buttons) if (keys_[binding.key]) pad.wButtons |= binding.button;
    if (keys_[VK_SPACE]) pad.wButtons |= mouseLook_ ? XINPUT_GAMEPAD_Y : XINPUT_GAMEPAD_A;
    if (keys_[VK_ESCAPE]) pad.wButtons |= escapePauses_ ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_B;
    auto axis = [&](unsigned negative, unsigned positive) -> SHORT {
        if (keys_[negative] == keys_[positive]) return 0;
        return keys_[negative] ? -32768 : 32767;
    };
    pad.sThumbLX = axis('A', 'D');
    pad.sThumbLY = axis('S', 'W');
    pad.sThumbRX = axis('J', 'L');
    pad.sThumbRY = axis('K', 'I');
    pad.bLeftTrigger = ((mouseLook_ && rightMouse_) || keys_['Z']) ? 255 : 0;
    pad.bRightTrigger = ((mouseLook_ && leftMouse_) || keys_['X']) ? 255 : 0;
    if (mouseLook_) {
        if (middleMouse_) pad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        const auto now = api_.ticks();
        if (now >= wheelNext_) {
            if (wheelButton_) { wheelButton_ = 0; wheelNext_ = now + 20; }
            else if (wheelPending_) {
                wheelButton_ = wheelPending_ > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
                wheelPending_ += wheelPending_ > 0 ? -1 : 1;
                wheelNext_ = now + 40;
            }
        }
        pad.wButtons |= wheelButton_;
    }
    return pad;
}
DWORD NativeInput::getState(Memory& owner, uint32_t user, uint32_t flags, uint32_t output) {
    if (user >= slots_.size() || flags != 0 || !guestSpan(owner, output, 16, true))
        return ERROR_INVALID_PARAMETER;
    std::lock_guard lock(mutex_);
    ++counters_.polls;
    XINPUT_STATE state{};
    DWORD status = api_.state(user, &state);
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
        if (!settingsOpen_ && neutral) waitForControllerRelease_[user] = false;
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
            keyboardMouseActive = heldKeys_ != 0 || keyboardMouseEvent_ || leftMouse_ || rightMouse_ ||
                middleMouse_ || mousePending_ || wheelPending_ != 0 ||
                wheelButton_ != 0 || wheelRemainder_ != 0;
        }
        updatePromptSourceLocked(user, state.Gamepad, status == ERROR_SUCCESS, keyboardMouseActive);
        if (user == 0) keyboardMouseEvent_ = false;
    }
    if (keyboard) mergeKeyboard(state.Gamepad, keyboardLocked());
    // Windows focus suppresses all input, including physical controllers.
    // Caps still report the attached device; returning neutral avoids inventing
    // a disconnect whenever the user switches to another application.
    if (settingsOpen_ || (window_ && !focused_)) state.Gamepad = {};
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
    std::lock_guard lock(mutex_);
    XINPUT_CAPABILITIES caps{};
    DWORD status = api_.capabilities(user, flags, &caps);
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
    std::lock_guard lock(mutex_);
    XINPUT_VIBRATION vibration{read16(owner.base() + input), read16(owner.base() + input + 2)};
    bool requested = vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed;
    if (settingsOpen_ || (window_ && !focused_)) vibration = {};
    DWORD status = api_.vibration(user, &vibration);
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
