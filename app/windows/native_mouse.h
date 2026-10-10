#pragma once
#include "runtime/native/input.h"
#include "runtime/native/menu_pointer.h"
#include "mouse_capture_policy.h"
#include <cstdio>
#include <cstdlib>
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
    void setRenderHeight(unsigned height) { renderHeight_ = height; updateTitle(); }
    void updateGameplayCapture(bool inputAvailable) {
        // The guest retires the outgoing menu before publishing this request.
        // Take it first so an older ownership sample cannot consume the event.
        const bool loadAccepted = DarkRecomp::Native::nativeInput().takeGameplayMouseCaptureRequest();
        const bool menuActive = DarkRecomp::Native::guestMenuPointerActive();
        const bool gameplayActive = !DarkRecomp::Native::nativeInput().guestMenuAllowsPointer();
        const bool available = inputAvailable && registered_ && GetForegroundWindow() == window_ && !IsIconic(window_);
        const bool keyboardMouse = DarkRecomp::Native::nativeInput().promptSource() ==
                                   DarkRecomp::Native::PromptInputSource::KeyboardMouse;
        const bool alreadyCaptured = captured_;
        const auto action = capturePolicy_.update(menuActive, gameplayActive, available, keyboardMouse, loadAccepted);
        if (action == NativeMouseCapturePolicy::Action::Release) releaseCapture();
        else if (action == NativeMouseCapturePolicy::Action::Capture && !captured_) capture(loadAccepted);
        if (loadAccepted && alreadyCaptured && captured_ && available && keyboardMouse)
            DarkRecomp::Native::nativeInput().setMouseLookEnabled(false, true);
    }
    bool capture(bool waitForClient = false) {
        if (DarkRecomp::Native::guestMenuPointerActive()) return false;
        if (!registered_ || GetForegroundWindow() != window_ || IsIconic(window_)) return false;
        capturePolicy_.cancel();
        if (!updateClip()) return false;
        captured_ = true;
        SetCapture(window_);
        if (GetCapture() != window_) { release(); return false; }
        DarkRecomp::Native::nativeInput().setMouseLookEnabled(!waitForClient, waitForClient);
        SetCursor(nullptr);
        updateTitle();
        if (captureProbeEnabled())
            std::printf("[MouseCapture] tick=%llu captured=1 waitingForClient=%u\n", GetTickCount64(), unsigned(waitForClient));
        return true;
    }
    void release() {
        capturePolicy_.cancel();
        DarkRecomp::Native::nativeInput().cancelGameplayMouseCaptureRequest();
        releaseCapture();
    }
    void suspend() {
        capturePolicy_.suspend();
        DarkRecomp::Native::nativeInput().cancelGameplayMouseCaptureRequest();
        releaseCapture();
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
        } else if (message == WM_LBUTTONDOWN && !captured_ && !DarkRecomp::Native::guestMenuPointerActive()) {
            capture(); return true; // Acquiring the mouse must not fire a shot.
        } else if (message == WM_KEYDOWN && !(detail & (LPARAM(1) << 30))) {
            if (key == VK_ESCAPE && captured_) release(); // Input already received Pause; menu Back keeps its resume request.
            else if (key == VK_F2) { if (captured_) release(); else capture(); return true; }
            else if (key == VK_F1) {
                release();
                MessageBoxW(window_,
                    L"Mouse captures when keyboard/mouse gameplay starts or resumes after a menu.\n"
                    L"Escape pauses and releases it. Click to recapture after a manual release.\n"
                    L"F2 captures/releases the mouse. Alt-Tab releases it automatically.\n"
                    L"F5: developer tools (mission, speed, invincibility, noclip and Darkness).\n\n"
                    L"F6: 720p/1440p resolution switch; enable this shortcut in F5 first.\n"
                    L"Availability depends on the launch resolution and display aspect.\n\n"
                    L"Options > Video Settings: graphics    Alt+Enter: fullscreen\n"
                    L"Bloom, motion blur, texture filtering, VSync, frame cap, resolution and field of view.\n\n"
                    L"Options > Controls > Keyboard bindings: customize keys and mouse buttons.\n"
                    L"Alt and mouse side buttons M4/M5 can be assigned.\n"
                    L"Default keyboard and mouse controls:\n"
                    L"WASD: move    Mouse: look    Space: jump\n"
                    L"E: use / confirm    R: reload    Ctrl or C: crouch\n"
                    L"Left click: fire right weapon    Right click: fire left weapon\n"
                    L"Middle click or Shift: zoom    Wheel or 1/2: switch weapons\n"
                    L"Wheel: previous/next dialogue choice or menu item\n"
                    L"Q: manifest Darkness    G: use Darkness power\n"
                    L"3/4: switch power    F: redirect Darkling\n"
                    L"Tab: journal    Enter: pause / Start\n\n"
                    L"Menus: move the cursor and click an item, or use arrows and E / Space.\n"
                    L"Esc or Backspace goes back.\n"
                    L"Space also skips the intro videos. Inversion uses the game options.\n"
                    L"Mouse sensitivity: launch with --mouse-sensitivity 1.0 (0.1 to 10).",
                    L"The Darkness - Keyboard and mouse controls", MB_OK);
                return true;
            }
        } else if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !key) ||
                   message == WM_ENTERSIZEMOVE || message == WM_ENTERMENULOOP ||
                   message == WM_CANCELMODE || message == WM_NCDESTROY ||
                   (message == WM_SYSKEYDOWN && key == VK_MENU)) suspend();
        else if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(detail) != window_ && captured_) suspend();
        else if ((message == WM_MOVE || message == WM_SIZE) && captured_) {
            if (IsIconic(window_) || !updateClip()) suspend();
        }
        return false;
    }
private:
    static bool captureProbeEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("DARK_MENU_POINTER_PROBE");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        return enabled;
    }
    void releaseCapture() {
        if (!captured_) return;
        captured_ = false;
        DarkRecomp::Native::nativeInput().setMouseLookEnabled(false);
        RECT current{};
        if (GetClipCursor(&current) && EqualRect(&current, &clip_)) ClipCursor(nullptr);
        // ReleaseCapture re-enters message() with WM_CAPTURECHANGED. Since
        // captured_ is already false, that notification must not cancel the
        // menu's pending automatic capture on resume.
        if (GetCapture() == window_) ReleaseCapture();
        if (GetForegroundWindow() == window_) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        updateTitle();
    }
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
        if (IsWindow(window_)) SetWindowTextW(window_, (title + L" | " + std::to_wstring(renderHeight_) +
            L"p | F5 developer tools | " + std::to_wstring(fps_) + L" FPS").c_str());
    }
    HWND window_{};
    RECT clip_{};
    bool registered_ = false, captured_ = false;
    NativeMouseCapturePolicy capturePolicy_;
    unsigned fps_ = 0;
    unsigned renderHeight_ = 720;
};
