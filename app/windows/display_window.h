#pragma once
#include <windows.h>
#include <stdexcept>

class NativeDisplayWindow {
    HWND window_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowedStyle_ = 0;
public:
    explicit NativeDisplayWindow(HWND window) : window_(window) {}
    bool fullscreen() const noexcept { return fullscreen_; }
    void toggleFullscreen() {
        if (!fullscreen_) {
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (!GetWindowPlacement(window_, &placement_) ||
                !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
                throw std::runtime_error("Cannot determine the game monitor dimensions");
            windowedStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
            SetWindowLongPtrW(window_, GWL_STYLE, (windowedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            const RECT& rect = monitor.rcMonitor;
            SetWindowPos(window_, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            fullscreen_ = true;
        } else {
            SetWindowLongPtrW(window_, GWL_STYLE, windowedStyle_);
            SetWindowPlacement(window_, &placement_);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            fullscreen_ = false;
        }
    }
};
