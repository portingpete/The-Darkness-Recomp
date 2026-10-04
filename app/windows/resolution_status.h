#pragma once
#include <windows.h>
#include <algorithm>
#include <string>

// A short, non-activating status message above the game image. Mouse input
// passes through to the game; no timer or repaint work runs while it is hidden.
class NativeResolutionStatus {
    HWND window_{};
    ULONGLONG until_{};
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM key, LPARAM detail) {
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            RECT bounds{}; GetClientRect(window, &bounds);
            FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
            const auto font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            wchar_t text[256]{}; GetWindowTextW(window, text, 256);
            bounds.left += 12; bounds.right -= 12;
            DrawTextW(dc, text, -1, &bounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, font); EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, key, detail);
    }
public:
    explicit NativeResolutionStatus(HWND parent) {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DarkRecompResolutionStatus";
        RegisterClassW(&type);
        window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            type.lpszClassName, L"", WS_CHILD, 20, 20, 480, 38,
            parent, nullptr, type.hInstance, nullptr);
    }
    ~NativeResolutionStatus() { if (IsWindow(window_)) DestroyWindow(window_); }
    void show(const std::wstring& text) {
        if (!window_) return;
        const auto parent = GetParent(window_);
        RECT bounds{}; GetClientRect(parent, &bounds);
        const auto width = (std::min)(480L, (std::max)(160L, bounds.right - 40));
        SetWindowTextW(window_, text.c_str());
        SetWindowPos(window_, HWND_TOP, 20, 20, width, 38, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(window_, nullptr, FALSE);
        until_ = GetTickCount64() + 3000;
    }
    void update() {
        if (until_ && GetTickCount64() >= until_) {
            ShowWindow(window_, SW_HIDE); until_ = 0;
        }
    }
};
