#include "keyboard_menu.h"
#include <algorithm>
#include <charconv>
#include <mutex>
#include <optional>

namespace DarkRecomp::Native {
namespace {
struct BindingSlot { size_t action, slot; };
std::optional<BindingSlot> bindingSlot(std::string_view action) noexcept {
    constexpr std::string_view prefix = "darkrecomp.keyboard.bind.";
    if (!action.starts_with(prefix)) return {};
    action.remove_prefix(prefix.size());
    const auto delimiter = action.find('.');
    if (delimiter == std::string_view::npos) return {};
    unsigned index = 0, slot = 0;
    const auto first = std::from_chars(action.data(), action.data() + delimiter, index);
    const auto second = std::from_chars(action.data() + delimiter + 1, action.data() + action.size(), slot);
    if (first.ec != std::errc{} || first.ptr != action.data() + delimiter ||
        second.ec != std::errc{} || second.ptr != action.data() + action.size() ||
        index >= kKeyboardActionCount || slot >= kKeyboardBindingSlots) return {};
    return BindingSlot{index, slot};
}
std::string keyLabel(unsigned key) {
    switch (key) {
    case 0: return "UNBOUND";
    case VK_CONTROL: case VK_LCONTROL: return "CTRL";
    case VK_RCONTROL: return "RCTRL";
    case VK_SHIFT: return "SHIFT";
    case VK_LSHIFT: return "LSHIFT";
    case VK_RSHIFT: return "RSHIFT";
    case VK_MENU: return "ALT";
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return "ENTER";
    case VK_BACK: return "BACKSP";
    case VK_PRIOR: return "PAGE UP";
    case VK_NEXT: return "PAGE DN";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    case VK_SNAPSHOT: return "PRTSC";
    case VK_NUMLOCK: return "NUMLOCK";
    case VK_SCROLL: return "SCROLL";
    case VK_CAPITAL: return "CAPSLOCK";
    case VK_OEM_COMMA: return "COMMA";
    case VK_OEM_PERIOD: return "PERIOD";
    case VK_OEM_1: return "SEMICOL";
    case VK_OEM_2: return "SLASH";
    case VK_OEM_3: return "GRAVE";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "BKSLASH";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "APOST";
    case VK_OEM_MINUS: return "MINUS";
    case VK_OEM_PLUS: return "EQUALS";
    default: break;
    }
    auto text = keyboardKeyPrompt(key);
    for (char& glyph : text) if (glyph >= 'a' && glyph <= 'z') glyph = char(glyph - 'a' + 'A');
    return text;
}
std::string centered(std::string text, size_t width) {
    if (text.size() > width) text.resize(width);
    const auto padding = width - text.size();
    return std::string(padding / 2, ' ') + text + std::string(padding - padding / 2, ' ');
}
std::string padded(std::string text, size_t width) {
    return "sc, " + centered(std::move(text), width);
}
std::mutex menuMutex;
KeyboardMenuState menu;
}

bool KeyboardMenuState::begin(const KeyboardBindings& current) noexcept {
    if (!validKeyboardBindings(current)) return false;
    if (active_) return true; // Switching original pages retains staged edits.
    staged_ = current;
    active_ = true;
    captureAction_ = kKeyboardActionCount;
    awaitRelease_ = closeRequested_ = closing_ = saveQueued_ = false;
    pendingId_ = 0;
    status_ = Status::Ready;
    return true;
}
void KeyboardMenuState::end() noexcept {
    active_ = false;
    captureAction_ = kKeyboardActionCount;
    awaitRelease_ = closeRequested_ = closing_ = saveQueued_ = false;
    pendingId_ = 0;
    status_ = Status::Ready;
}
bool KeyboardMenuState::action(std::string_view action) noexcept {
    const auto selected = bindingSlot(action);
    const bool defaults = action == "darkrecomp.keyboard.defaults";
    const bool save = action == "darkrecomp.keyboard.save";
    const bool cancel = action == "darkrecomp.keyboard.cancel";
    if (!selected && !defaults && !save && !cancel) return false;
    if (!active_ || pendingId_ || closing_) return true;
    if (selected) {
        captureAction_ = selected->action; captureSlot_ = selected->slot;
        awaitRelease_ = std::any_of(held_.begin(), held_.end(), [](bool down) { return down; });
        status_ = awaitRelease_ ? Status::Release : Status::Capture;
    } else if (defaults) {
        captureAction_ = kKeyboardActionCount; awaitRelease_ = false;
        staged_ = defaultKeyboardBindings(); status_ = Status::Defaults;
    } else if (cancel) {
        captureAction_ = kKeyboardActionCount; awaitRelease_ = false;
        closeRequested_ = closing_ = true;
    } else if (!captureActive()) {
        if (++nextId_ == 0) ++nextId_;
        pendingId_ = nextId_; saveQueued_ = true; status_ = Status::Saving;
    }
    return true;
}
std::string KeyboardMenuState::label(std::string_view action) const {
    if (const auto selected = bindingSlot(action)) {
        std::string text;
        if (captureActive() && captureAction_ == selected->action && captureSlot_ == selected->slot)
            text = awaitRelease_ ? "RELEASE" : "PRESSKEY";
        else text = keyLabel(staged_.keys[selected->action][selected->slot]);
        // CubeButton trims whitespace while sizing its initial TEXT. Keep
        // visible edges so every key/capture label owns the full five cells.
        return "sc, [" + centered(std::move(text), 8) + "]";
    }
    if (action == "darkrecomp.keyboard.save")
        return active_ && !closing_ && status_ == Status::Failed ? "sc, FAIL" : "sc, SAVE";
    if (action != "darkrecomp.keyboard.status") return {};
    const char* text = "SAVE APPLIES CHANGES. CANCEL DISCARDS.";
    switch (status_) {
    case Status::Ready: break;
    case Status::Release: text = "RELEASE HELD KEYS/BUTTONS, THEN PRESS."; break;
    case Status::Capture: text = "PRESS A KEY/MOUSE; ESC CANCEL, DEL CLEAR"; break;
    case Status::Reserved: text = "RESERVED KEY. TRY AGAIN; ESC CANCELS."; break;
    case Status::Updated: text = "BINDING UPDATED. SAVE TO APPLY."; break;
    case Status::Defaults: text = "DEFAULTS STAGED. SAVE TO APPLY."; break;
    case Status::Saving: text = "SAVING KEYBOARD/MOUSE BINDINGS..."; break;
    case Status::Failed: text = "SAVE FAILED. TRY AGAIN OR CANCEL."; break;
    case Status::Saved: text = "KEYBOARD/MOUSE BINDINGS SAVED."; break;
    }
    return padded(text, 40);
}
bool KeyboardMenuState::keyEvent(unsigned key, bool down, bool repeat, bool alt) noexcept {
    if (key >= held_.size() || key == 0) return false;
    const bool wasDown = held_[key];
    held_[key] = down;
    if (active_ && (pendingId_ || closing_)) return true;
    if (!captureActive()) return false;
    if (down && !repeat && !wasDown && key == VK_ESCAPE) {
        captureAction_ = kKeyboardActionCount; awaitRelease_ = false; status_ = Status::Ready;
        return true;
    }
    if (awaitRelease_) {
        if (!std::any_of(held_.begin(), held_.end(), [](bool held) { return held; })) {
            awaitRelease_ = false; status_ = Status::Capture;
        }
        return true;
    }
    if (!down || repeat || wasDown) return true;
    // Alt itself arrives as a system key with the Alt context bit set. Other
    // keys pressed with Alt remain reserved shortcut combinations.
    if ((alt && key != VK_MENU) || !assignableKeyboardKey(key)) { status_ = Status::Reserved; return true; }
    const unsigned selectedKey = key == VK_DELETE ? 0 : key;
    assignKeyboardKey(staged_, KeyboardAction(captureAction_), captureSlot_, selectedKey);
    captureAction_ = kKeyboardActionCount; status_ = Status::Updated;
    return true;
}
void KeyboardMenuState::cancelCapture() noexcept {
    held_.fill(false);
    captureAction_ = kKeyboardActionCount; awaitRelease_ = false;
    if (!pendingId_) status_ = Status::Ready;
}
bool KeyboardMenuState::takeSaveRequest(KeyboardMenuSaveRequest& request) noexcept {
    if (!active_ || !pendingId_ || !saveQueued_) return false;
    request = {pendingId_, staged_}; saveQueued_ = false;
    return true;
}
void KeyboardMenuState::reportSave(uint64_t id, bool saved) noexcept {
    if (!active_ || !id || pendingId_ != id) return;
    pendingId_ = 0; saveQueued_ = false;
    status_ = saved ? Status::Saved : Status::Failed;
    if (saved) closeRequested_ = closing_ = true;
}
bool KeyboardMenuState::takeCloseRequest() noexcept {
    if (!active_ || !closeRequested_) return false;
    closeRequested_ = false;
    return true;
}

bool beginKeyboardMenu(const KeyboardBindings& current) noexcept { std::lock_guard lock(menuMutex); return menu.begin(current); }
void endKeyboardMenu() noexcept { std::lock_guard lock(menuMutex); menu.end(); }
bool keyboardMenuAction(std::string_view action) noexcept { std::lock_guard lock(menuMutex); return menu.action(action); }
std::string keyboardMenuLabel(std::string_view action) { std::lock_guard lock(menuMutex); return menu.label(action); }
bool keyboardMenuCaptureActive() noexcept { std::lock_guard lock(menuMutex); return menu.captureActive(); }
bool keyboardMenuInputBlocked() noexcept { std::lock_guard lock(menuMutex); return menu.inputBlocked(); }
bool keyboardMenuKeyEvent(unsigned key, bool down, bool repeat, bool alt) noexcept { std::lock_guard lock(menuMutex); return menu.keyEvent(key, down, repeat, alt); }
void cancelKeyboardMenuCapture() noexcept { std::lock_guard lock(menuMutex); menu.cancelCapture(); }
bool takeKeyboardMenuSaveRequest(KeyboardMenuSaveRequest& request) noexcept { std::lock_guard lock(menuMutex); return menu.takeSaveRequest(request); }
void reportKeyboardMenuSave(uint64_t id, bool saved) noexcept { std::lock_guard lock(menuMutex); menu.reportSave(id, saved); }
bool takeKeyboardMenuCloseRequest() noexcept { std::lock_guard lock(menuMutex); return menu.takeCloseRequest(); }
}
