#pragma once

// Menu ownership, accepted gameplay loads and the original gameplay-client
// signal arrive independently. Keep the request through loading, but never
// infer gameplay from an idle menu pointer alone (intro videos and animated
// menu transitions can both be idle).
class NativeMouseCapturePolicy {
public:
    enum class Action { None, Release, Capture };
    Action update(bool menuActive, bool gameplayActive, bool available, bool keyboardMouse,
                  bool loadAccepted = false) noexcept {
        if (menuActive && available && (!menuWasActive_ || !wasAvailable_)) pending_ = true;
        menuWasActive_ = menuActive;
        wasAvailable_ = available;
        if (!available) pending_ = false;
        if (menuActive) return Action::Release;
        if (!pending_ || (!gameplayActive && !loadAccepted)) return Action::None;
        pending_ = false;
        return keyboardMouse ? Action::Capture : Action::None;
    }
    // Explicit release, focus loss and native panels retire this transition.
    // Regaining focus in gameplay must not take the desktop mouse. Refocusing
    // an open menu can arm its next Start/resume transition.
    void cancel() noexcept { pending_ = false; }
    // Window operations may enter and exit a nested Win32 message loop before
    // the next update. Let an available menu rearm even if unavailability was
    // never sampled, while gameplay still requires a fresh menu transition.
    void suspend() noexcept { pending_ = false; wasAvailable_ = false; }
private:
    bool menuWasActive_ = false, wasAvailable_ = false, pending_ = false;
};
