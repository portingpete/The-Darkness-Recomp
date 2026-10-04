#pragma once
#include "keyboard_bindings.h"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace DarkRecomp::Native {
struct KeyboardMenuSaveRequest {
    uint64_t id = 0;
    KeyboardBindings bindings{};
};

// The model owns staged bindings only. Original Cube menus own navigation;
// the host owns persistence and applying a successfully saved selection.
class KeyboardMenuState {
public:
    bool begin(const KeyboardBindings& current) noexcept;
    void end() noexcept;
    bool action(std::string_view action) noexcept;
    std::string label(std::string_view action) const;
    // Send all physical keyboard edges, including while this menu is closed.
    // This lets capture wait for the key which activated its original button.
    bool keyEvent(unsigned key, bool down, bool repeat = false, bool alt = false) noexcept;
    void cancelCapture() noexcept;
    bool takeSaveRequest(KeyboardMenuSaveRequest& request) noexcept;
    void reportSave(uint64_t id, bool saved) noexcept;
    bool takeCloseRequest() noexcept;
    bool active() const noexcept { return active_; }
    bool captureActive() const noexcept { return active_ && captureAction_ < kKeyboardActionCount; }
    bool saving() const noexcept { return pendingId_ != 0; }
    bool inputBlocked() const noexcept { return active_ && (captureActive() || saving() || closing_); }
    const KeyboardBindings& staged() const noexcept { return staged_; }
private:
    enum class Status { Ready, Release, Capture, Reserved, Updated, Defaults, Saving, Failed, Saved };
    KeyboardBindings staged_ = defaultKeyboardBindings();
    std::array<bool, 256> held_{};
    size_t captureAction_ = kKeyboardActionCount, captureSlot_ = 0;
    bool active_ = false, awaitRelease_ = false, closeRequested_ = false, closing_ = false, saveQueued_ = false;
    uint64_t nextId_ = 0, pendingId_ = 0;
    Status status_ = Status::Ready;
};

// Both original guest callbacks and Win32 input use this synchronized bridge.
bool beginKeyboardMenu(const KeyboardBindings& current) noexcept;
void endKeyboardMenu() noexcept;
bool keyboardMenuAction(std::string_view action) noexcept;
std::string keyboardMenuLabel(std::string_view action);
bool keyboardMenuCaptureActive() noexcept;
bool keyboardMenuInputBlocked() noexcept;
bool keyboardMenuKeyEvent(unsigned key, bool down, bool repeat = false, bool alt = false) noexcept;
void cancelKeyboardMenuCapture() noexcept;
bool takeKeyboardMenuSaveRequest(KeyboardMenuSaveRequest& request) noexcept;
void reportKeyboardMenuSave(uint64_t id, bool saved) noexcept;
// No guest pointer crosses threads. The current owning FrontEnd consumes this
// request and uses its original deferred cg_prevmenu callback on the UI thread.
bool takeKeyboardMenuCloseRequest() noexcept;
}
