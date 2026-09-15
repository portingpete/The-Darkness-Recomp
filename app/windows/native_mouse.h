#pragma once
#include "runtime/native/input.h"
#include <string>

// The window thread owns Win32 capture. The input mutex is never held across
// SetCapture/ReleaseCapture, which may synchronously re-enter the window proc.
class NativeMouseWindow {
public:
    explicit NativeMouseWindow(HWND window) : window_(window) {
        RAWINPUTDEVICE mouse{0x01, 0x02, 0, window}; // Foreground mouse only.
        registered_ = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
        updateTitle();
    }
    ~NativeMouseWindow() {
        release();
        if (registered_) {
            RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        }
    }
    bool registered() const { return registered_; }
    bool captured() const { return captured_; }
    void setFrameRate(double fps) { fps_ = unsigned(fps + .5); updateTitle(); }
    bool capture() {
        if (!registered_ || GetForegroundWindow() != window_ || IsIconic(window_)) return false;
        if (!updateClip()) return false;
        captured_ = true;
        SetCapture(window_);
        if (GetCapture() != window_) { release(); return false; }
        DarkRecomp::Native::nativeInput().setMouseLookEnabled(true);
        SetCursor(nullptr);
        updateTitle();
        return true;
    }
    void release() {
        if (!captured_) return;
        captured_ = false;
        DarkRecomp::Native::nativeInput().setMouseLookEnabled(false);
        RECT current{};
        if (GetClipCursor(&current) && EqualRect(&current, &clip_)) ClipCursor(nullptr);
        if (GetCapture() == window_) ReleaseCapture();
        if (GetForegroundWindow() == window_) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        updateTitle();
    }
    bool message(UINT message, WPARAM key, LPARAM detail) {
        if (message == WM_INPUT) {
            RAWINPUT raw{}; UINT size = sizeof(raw);
            const auto count = GetRawInputData(reinterpret_cast<HRAWINPUT>(detail), RID_INPUT,
                                               &raw, &size, sizeof(RAWINPUTHEADER));
            if (captured_ && count != UINT(-1) && count >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) &&
                raw.header.dwType == RIM_TYPEMOUSE && raw.header.dwSize <= count &&
                !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                DarkRecomp::Native::nativeInput().mouseMotion(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
            // Foreground WM_INPUT still reaches DefWindowProc for cleanup.
        } else if (message == WM_SETCURSOR && captured_ && LOWORD(detail) == HTCLIENT) {
            SetCursor(nullptr); return true;
        } else if (message == WM_LBUTTONDOWN && !captured_) {
            capture(); return true; // Acquiring the mouse must not fire a shot.
        } else if (message == WM_KEYDOWN && !(detail & (LPARAM(1) << 30))) {
            if (key == VK_ESCAPE) release(); // Input already received Pause.
            else if (key == VK_F2) { if (captured_) release(); else capture(); return true; }
            else if (key == VK_F1) {
                release();
                MessageBoxW(window_,
                    L"Click in the window to play. Escape pauses and releases the mouse.\n"
                    L"F2 captures/releases the mouse. Alt-Tab releases it automatically.\n\n"
                    L"Options > Video Settings: graphics    Alt+Enter: fullscreen\n"
                    L"Bloom, motion blur, VSync, frame cap, resolution, fullscreen and field of view.\n\n"
                    L"WASD: move    Mouse: look    Space: jump\n"
                    L"E: use / confirm    R: reload    Ctrl or C: crouch\n"
                    L"Left click: fire right weapon    Right click: fire left weapon\n"
                    L"Middle click or Shift: zoom    Wheel or 1/2: switch weapons\n"
                    L"Q: manifest Darkness    G: use Darkness power\n"
                    L"3/4: switch power    F: redirect Darkling\n"
                    L"Tab: journal    Enter: pause / Start\n\n"
                    L"Menus (mouse released): arrows, E or Space to confirm, Esc or Backspace to go back.\n"
                    L"Space also skips the intro videos. Inversion uses the game options.\n"
                    L"Mouse sensitivity: launch with --mouse-sensitivity 1.0 (0.1 to 10).",
                    L"The Darkness - Keyboard and mouse controls", MB_OK);
                return true;
            }
        } else if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !key) ||
                   message == WM_ENTERSIZEMOVE || message == WM_ENTERMENULOOP ||
                   message == WM_CANCELMODE || message == WM_NCDESTROY ||
                   (message == WM_SYSKEYDOWN && key == VK_MENU)) release();
        else if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(detail) != window_) release();
        else if ((message == WM_MOVE || message == WM_SIZE) && captured_) {
            if (IsIconic(window_) || !updateClip()) release();
        }
        return false;
    }
private:
    bool updateClip() {
        RECT rect{};
        if (!GetClientRect(window_, &rect) || IsRectEmpty(&rect)) return false;
        MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&rect), 2);
        if (!ClipCursor(&rect)) return false;
        clip_ = rect;
        return true;
    }
    void updateTitle() {
        const std::wstring title = captured_ ?
            L"The Darkness - Mouse look | Esc releases | F1 controls" :
            L"The Darkness - Click to play | F1 controls | F2 mouse capture";
        if (IsWindow(window_)) SetWindowTextW(window_, (title + L" | " + std::to_wstring(fps_) + L" FPS").c_str());
    }
    HWND window_{};
    RECT clip_{};
    bool registered_ = false, captured_ = false;
    unsigned fps_ = 0;
};
