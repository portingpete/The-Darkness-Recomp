#pragma once
#include <windows.h>

// Display-thread state for the optional F6 shortcut. Requests are consumed only
// between completed renderer frames; disabling the shortcut drops queued work.
class DeveloperResolutionShortcut {
public:
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled) {
        enabled_ = enabled;
        if (!enabled_) pending_ = false;
    }

    bool handle(UINT message, WPARAM key, LPARAM detail) {
        if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !key)) {
            down_ = false;
            return false;
        }
        if (key != VK_F6 || (message != WM_KEYDOWN && message != WM_KEYUP)) return false;
        if (message == WM_KEYUP) {
            down_ = false;
        } else {
            const bool repeated = (detail & (LPARAM(1) << 30)) != 0;
            if (!down_ && !repeated && enabled_) pending_ = !pending_;
            down_ = true;
        }
        // F6 stays reserved from gameplay even when the shortcut is disabled.
        return true;
    }

    bool takeToggle() {
        const bool pending = pending_;
        pending_ = false;
        return pending;
    }

private:
    bool enabled_ = false;
    bool down_ = false;
    bool pending_ = false;
};
