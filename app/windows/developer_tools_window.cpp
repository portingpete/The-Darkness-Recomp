#include "developer_tools_window.h"
#include "native_mouse.h"
#include "runtime/native/developer_tools.h"
#include "runtime/native/input.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace DarkRecomp::Native;

namespace {
constexpr wchar_t kWindowClass[] = L"DarkRecompDeveloperToolsWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kMission = 1001, kLoad = 1002, kSpeed = 1003, kInvincible = 1004, kDefaults = 1005;
constexpr int kNoclip = 1006, kUnlock = 1007, kMaxDarkness = 1008, kResolutionShortcut = 1009;
constexpr std::array<float, 9> kSpeeds{.25f, .5f, .75f, 1.f, 1.25f, 1.5f, 2.f, 3.f, 4.f};
constexpr std::array<const wchar_t*, 9> kSpeedNames{
    L"0.25X", L"0.5X", L"0.75X", L"1X (NORMAL)", L"1.25X", L"1.5X", L"2X", L"3X", L"4X"};

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!size) return L"Status text unavailable.";
    std::wstring converted(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), converted.data(), size);
    return converted;
}

std::wstring upperWide(std::string_view text) {
    auto converted = wide(text);
    if (!converted.empty()) CharUpperBuffW(converted.data(), DWORD(converted.size()));
    return converted;
}

HWND control(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
             int x, int y, int width, int height, int id = 0) {
    const HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    if (!result) throw std::runtime_error("Cannot create developer-tools control");
    SendMessageW(result, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return result;
}
}

DeveloperToolsWindow::DeveloperToolsWindow(HWND owner, NativeMouseWindow& mouse,
    std::function<void(bool)> resolutionShortcut)
    : owner_(owner), mouse_(mouse), setResolutionShortcut_(std::move(resolutionShortcut)) {}
DeveloperToolsWindow::~DeveloperToolsWindow() { close(); releaseInputGate(); }

bool DeveloperToolsWindow::handleMessage(MSG& message) {
    const bool ours = message.hwnd == owner_ || (window_ &&
        (message.hwnd == window_ || IsChild(window_, message.hwnd)));
    if (ours && (message.message == WM_KEYDOWN || message.message == WM_KEYUP) && message.wParam == VK_F5) {
        if (message.message == WM_KEYDOWN && !(message.lParam & (LPARAM(1) << 30))) {
            const bool wasOpen = isOpen();
            toggle();
            std::printf("[DeveloperTools] F5 handled phase=down source=%s before=%u after=%u\n",
                message.hwnd == owner_ ? "owner" : "panel", unsigned(wasOpen), unsigned(isOpen()));
        } else if (message.message == WM_KEYUP) {
            std::printf("[DeveloperTools] F5 handled phase=up open=%u\n", unsigned(isOpen()));
        }
        return true;
    }
    if (window_ && (message.hwnd == window_ || IsChild(window_, message.hwnd)) &&
        message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        close();
        return true;
    }
    return window_ && (message.hwnd == window_ || IsChild(window_, message.hwnd)) &&
        IsDialogMessageW(window_, &message);
}

void DeveloperToolsWindow::toggle() { if (window_) close(); else open(); }

bool DeveloperToolsWindow::open() {
    if (window_) return true;
    setDeveloperToolsVisible(false);
    if (!IsWindow(owner_)) return false;
    mouse_.release();
    nativeInput().setSettingsOpen(true);
    inputGate_ = true;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        releaseInputGate();
        std::fputs("[DeveloperTools] Cannot register panel window.\n", stderr);
        return false;
    }
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD extended = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW;
    RECT size{0, 0, 450, 452};
    AdjustWindowRectEx(&size, style, FALSE, extended);
    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    const int width = size.right - size.left, height = size.bottom - size.top;
    int x = ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2;
    int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        x = (std::max)(monitor.rcWork.left, (std::min)(LONG(x), monitor.rcWork.right - width));
        y = (std::max)(monitor.rcWork.top, (std::min)(LONG(y), monitor.rcWork.bottom - height));
    }
    const HWND created = CreateWindowExW(extended, kWindowClass, L"THE DARKNESS - DEVELOPER TOOLS",
        style, x, y, width, height, owner_, nullptr, windowClass.hInstance, this);
    if (!created) {
        releaseInputGate();
        std::fputs("[DeveloperTools] Cannot create panel window.\n", stderr);
        return false;
    }
    setDeveloperToolsVisible(true);
    refresh();
    SetTimer(window_, kRefreshTimer, 250, nullptr);
    ShowWindow(window_, SW_SHOW);
    SetFocus(mission_);
    std::puts("[DeveloperTools] Panel opened; game input suspended. F5 or Escape closes it.");
    return true;
}

void DeveloperToolsWindow::close() {
    setDeveloperToolsVisible(false);
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    else releaseInputGate();
}

void DeveloperToolsWindow::releaseInputGate() {
    if (!inputGate_) return;
    inputGate_ = false;
    nativeInput().setSettingsOpen(false);
}

void DeveloperToolsWindow::createControls() {
    control(window_, L"STATIC", L"MISSION", 0, 16, 17, 330, 18);
    const DWORD comboStyle = CBS_DROPDOWNLIST | CBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP;
    mission_ = control(window_, L"COMBOBOX", L"", comboStyle, 16, 39, 327, 320, kMission);
    for (const auto& item : developerMissions()) {
        const auto title = upperWide(item.title);
        SendMessageW(mission_, CB_ADDSTRING, 0, LPARAM(title.c_str()));
    }
    SendMessageW(mission_, CB_SETCURSEL, 0, 0);
    load_ = control(window_, L"BUTTON", L"LOAD", BS_PUSHBUTTON | WS_TABSTOP, 355, 39, 78, 26, kLoad);
    control(window_, L"STATIC", L"PLAYER SPEED", 0, 16, 84, 180, 18);
    speed_ = control(window_, L"COMBOBOX", L"", comboStyle, 16, 106, 180, 240, kSpeed);
    for (const auto* text : kSpeedNames) SendMessageW(speed_, CB_ADDSTRING, 0, LPARAM(text));
    invincible_ = control(window_, L"BUTTON", L"INVINCIBLE", BS_AUTOCHECKBOX | WS_TABSTOP, 220, 106, 210, 26, kInvincible);
    noclip_ = control(window_, L"BUTTON", L"NOCLIP", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 140, 416, 26, kNoclip);
    unlock_ = control(window_, L"BUTTON", L"UNLOCK ALL DARKNESS", BS_PUSHBUTTON | WS_TABSTOP, 16, 181, 203, 27, kUnlock);
    maxDarkness_ = control(window_, L"BUTTON", L"MAX DARKNESS LEVEL", BS_PUSHBUTTON | WS_TABSTOP, 229, 181, 203, 27, kMaxDarkness);
    resolutionShortcut_ = control(window_, L"BUTTON", L"ENABLE F6 RESOLUTION SHORTCUT", BS_AUTOCHECKBOX | WS_TABSTOP,
                                  16, 223, 416, 26, kResolutionShortcut);
    control(window_, L"STATIC", L"STATUS", 0, 16, 265, 400, 18);
    status_ = control(window_, L"STATIC", L"", SS_LEFT, 16, 287, 416, 61);
    control(window_, L"STATIC", L"SPEED, INVINCIBILITY, NOCLIP AND THE F6 SHORTCUT ARE SESSION ONLY. MISSION LOADS AND DARKNESS GRANTS MAY AUTOSAVE.",
            0, 16, 357, 416, 42);
    defaults_ = control(window_, L"BUTTON", L"RESTORE DEFAULTS", BS_PUSHBUTTON | WS_TABSTOP, 16, 412, 150, 27, kDefaults);
    control(window_, L"BUTTON", L"CLOSE", BS_PUSHBUTTON | WS_TABSTOP, 355, 412, 78, 27, IDCANCEL);
}

void DeveloperToolsWindow::refresh() {
    if (!window_ || !status_) return;
    const auto snapshot = developerSnapshot();
    EnableWindow(mission_, snapshot.canLoadMission);
    EnableWindow(load_, snapshot.canLoadMission);
    EnableWindow(speed_, snapshot.hasActivePlayer);
    EnableWindow(invincible_, snapshot.hasActivePlayer);
    EnableWindow(noclip_, snapshot.hasActivePlayer);
    EnableWindow(unlock_, snapshot.hasActivePlayer);
    EnableWindow(maxDarkness_, snapshot.hasActivePlayer);
    EnableWindow(resolutionShortcut_, bool(setResolutionShortcut_));
    const auto speed = std::find(kSpeeds.begin(), kSpeeds.end(), snapshot.playerSpeed);
    SendMessageW(speed_, CB_SETCURSEL, speed == kSpeeds.end() ? -1 : speed - kSpeeds.begin(), 0);
    SendMessageW(invincible_, BM_SETCHECK, snapshot.invincible ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(noclip_, BM_SETCHECK, snapshot.noclip ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(resolutionShortcut_, BM_SETCHECK, resolutionShortcutEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    std::wstring status = upperWide(snapshot.status);
    if (!snapshot.hasActivePlayer) status += L"\nPLAYER CONTROLS BECOME AVAILABLE WHEN A PLAYER IS ACTIVE.";
    if (status != displayedStatus_) {
        displayedStatus_ = std::move(status);
        SetWindowTextW(status_, displayedStatus_.c_str());
    }
}

void DeveloperToolsWindow::loadMission() {
    if (!developerSnapshot().canLoadMission) { refresh(); return; }
    const LRESULT selected = SendMessageW(mission_, CB_GETCURSEL, 0, 0);
    const auto missions = developerMissions();
    if (selected >= 0 && size_t(selected) < missions.size() &&
        requestDeveloperMission(missions[size_t(selected)].id)) {
        // Return focus/input before the original loading movie and mission
        // opening sequence begin. F5 can reopen the panel after the transition.
        close();
    } else refresh();
}

void DeveloperToolsWindow::selectSpeed() {
    if (!developerSnapshot().hasActivePlayer) { refresh(); return; }
    const LRESULT selected = SendMessageW(speed_, CB_GETCURSEL, 0, 0);
    if (selected >= 0 && size_t(selected) < kSpeeds.size()) requestDeveloperSpeed(kSpeeds[size_t(selected)]);
    refresh();
}

void DeveloperToolsWindow::selectInvincibility() {
    if (!developerSnapshot().hasActivePlayer) { refresh(); return; }
    requestDeveloperInvincibility(SendMessageW(invincible_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    refresh();
}

void DeveloperToolsWindow::selectNoclip() {
    if (!developerSnapshot().hasActivePlayer) { refresh(); return; }
    requestDeveloperNoclip(SendMessageW(noclip_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    refresh();
}

void DeveloperToolsWindow::selectResolutionShortcut() {
    if (!setResolutionShortcut_) return;
    resolutionShortcutEnabled_ = SendMessageW(resolutionShortcut_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    setResolutionShortcut_(resolutionShortcutEnabled_);
    refresh();
}

void DeveloperToolsWindow::unlockDarkness() {
    if (developerSnapshot().hasActivePlayer) requestDeveloperUnlockDarkness();
    refresh();
}

void DeveloperToolsWindow::maxDarkness() {
    if (developerSnapshot().hasActivePlayer) requestDeveloperMaxDarkness();
    refresh();
}

void DeveloperToolsWindow::restoreDefaults() {
    requestDeveloperSpeed(1.f);
    requestDeveloperInvincibility(false);
    requestDeveloperNoclip(false);
    resolutionShortcutEnabled_ = false;
    if (setResolutionShortcut_) setResolutionShortcut_(false);
    refresh();
}

LRESULT CALLBACK DeveloperToolsWindow::windowProc(HWND window, UINT message, WPARAM key, LPARAM detail) {
    auto* self = reinterpret_cast<DeveloperToolsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<DeveloperToolsWindow*>(reinterpret_cast<CREATESTRUCTW*>(detail)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(self));
    }
    if (self) {
        try { return self->message(message, key, detail); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[DeveloperTools] %s\n", error.what());
            if (message == WM_CREATE) return -1;
        }
    }
    return DefWindowProcW(window, message, key, detail);
}

LRESULT DeveloperToolsWindow::message(UINT message, WPARAM key, LPARAM detail) {
    if (message == WM_CREATE) { createControls(); return 0; }
    if (message == WM_TIMER && key == kRefreshTimer) { refresh(); return 0; }
    if (message == WM_COMMAND) {
        const int id = LOWORD(key), notification = HIWORD(key);
        if (id == IDCANCEL && notification == BN_CLICKED) close();
        else if (id == kLoad && notification == BN_CLICKED) loadMission();
        else if (id == kSpeed && notification == CBN_SELCHANGE) selectSpeed();
        else if (id == kInvincible && notification == BN_CLICKED) selectInvincibility();
        else if (id == kNoclip && notification == BN_CLICKED) selectNoclip();
        else if (id == kUnlock && notification == BN_CLICKED) unlockDarkness();
        else if (id == kMaxDarkness && notification == BN_CLICKED) maxDarkness();
        else if (id == kResolutionShortcut && notification == BN_CLICKED) selectResolutionShortcut();
        else if (id == kDefaults && notification == BN_CLICKED) restoreDefaults();
        return 0;
    }
    if (message == WM_CLOSE) { close(); return 0; }
    if (message == WM_DESTROY) { setDeveloperToolsVisible(false); return 0; }
    if (message == WM_NCDESTROY) {
        setDeveloperToolsVisible(false);
        const HWND closed = window_;
        const HWND foreground = GetForegroundWindow();
        const bool restoreFocus = foreground == closed || foreground == owner_;
        KillTimer(closed, kRefreshTimer);
        SetWindowLongPtrW(closed, GWLP_USERDATA, 0);
        window_ = mission_ = load_ = speed_ = invincible_ = noclip_ = unlock_ = maxDarkness_ =
                  resolutionShortcut_ = defaults_ = status_ = nullptr;
        displayedStatus_.clear();
        releaseInputGate();
        if (restoreFocus && IsWindow(owner_)) SetFocus(owner_);
        std::puts("[DeveloperTools] Panel closed; click the game to capture the mouse.");
        return DefWindowProcW(closed, message, key, detail);
    }
    return DefWindowProcW(window_, message, key, detail);
}

void DeveloperToolsWindow::printStatus(std::string_view action) const {
    const auto snapshot = developerSnapshot();
    std::printf("[DeveloperTest] action=%.*s open=%u canLoadMission=%u hasActivePlayer=%u speed=%.2f invincible=%u noclip=%u resolutionShortcut=%u revision=%llu status=%s\n",
        int(action.size()), action.data(), unsigned(isOpen()), unsigned(snapshot.canLoadMission),
        unsigned(snapshot.hasActivePlayer), snapshot.playerSpeed, unsigned(snapshot.invincible), unsigned(snapshot.noclip),
        unsigned(resolutionShortcutEnabled_),
        static_cast<unsigned long long>(snapshot.revision), snapshot.status.c_str());
}

bool DeveloperToolsWindow::capturePanel(std::string_view path) {
    if (!window_ || !IsWindow(window_)) {
        std::fputs("[DeveloperTest] Panel capture unavailable while closed.\n", stderr);
        return false;
    }
    refresh();
    UpdateWindow(window_);
    RECT rectangle{};
    if (!GetWindowRect(window_, &rectangle)) return false;
    const int width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    const auto target = std::filesystem::path(wide(path));
    if (!target.is_absolute()) return false;
    HDC source = GetDC(window_), destination = source ? CreateCompatibleDC(source) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = destination ? CreateDIBSection(destination, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    struct Release {
        HWND window; HDC source, destination; HBITMAP bitmap; HGDIOBJ previous = nullptr;
        ~Release() {
            if (previous) SelectObject(destination, previous);
            if (bitmap) DeleteObject(bitmap);
            if (destination) DeleteDC(destination);
            if (source) ReleaseDC(window, source);
        }
    } release{window_, source, destination, bitmap};
    if (!bitmap || !pixels) return false;
    release.previous = SelectObject(destination, bitmap);
    // Capture only this process's owned developer panel. No desktop or game
    // assets/saves are read; PrintWindow draws its controls into the bitmap.
    if (!PrintWindow(window_, destination, 0)) return false;
    GdiFlush();
    const DWORD bytes = DWORD(width) * DWORD(height) * 4;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + bytes;
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels), bytes);
    output.close();
    return bool(output);
}

bool DeveloperToolsWindow::handleTestCommand(std::string_view command) {
    std::istringstream fields{std::string(command)};
    std::string prefix, action, value, extra;
    if (!(fields >> prefix >> action) || prefix != "dev") return false;
    if (action == "open" || action == "close" || action == "status" || action == "defaults") {
        if (fields >> extra) return false;
        if (action == "open") open();
        else if (action == "close") close();
        else if (action == "defaults") {
            if (window_) SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kDefaults, BN_CLICKED), LPARAM(defaults_));
            else std::puts("[DeveloperTest] Action unavailable while the panel is closed; use 'dev open' first.");
        }
        printStatus(action);
        return true;
    }
    if (!(fields >> value) || fields >> extra) return false;
    if (action == "capture") {
        if (!std::filesystem::path(wide(value)).is_absolute()) return false;
        const bool saved = capturePanel(value);
        std::printf("[DeveloperTest] Panel capture saved=%u path=%s\n", unsigned(saved), value.c_str());
        printStatus(action);
        return true;
    }
    size_t selected = 0;
    if (action == "mission") {
        const auto missions = developerMissions();
        const auto found = std::find_if(missions.begin(), missions.end(), [&](const auto& item) { return item.id == value; });
        if (found == missions.end()) return false;
        selected = size_t(found - missions.begin());
    } else if (action == "speed") {
        char* end = nullptr;
        const float multiplier = std::strtof(value.c_str(), &end);
        const auto found = std::find(kSpeeds.begin(), kSpeeds.end(), multiplier);
        if (end == value.c_str() || *end || !std::isfinite(multiplier) || found == kSpeeds.end()) return false;
        selected = size_t(found - kSpeeds.begin());
    } else if (action == "invincible" || action == "noclip" || action == "resolution") {
        if (value != "on" && value != "off") return false;
    } else if (action == "darkness") {
        if (value != "unlock" && value != "max") return false;
    } else return false;
    // Opening is explicit so diagnostics also exercise focus/capture lifecycle.
    if (!window_) {
        std::puts("[DeveloperTest] Action unavailable while the panel is closed; use 'dev open' first.");
    } else {
        refresh();
        if (action == "mission" && IsWindowEnabled(load_)) {
            SendMessageW(mission_, CB_SETCURSEL, selected, 0);
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kLoad, BN_CLICKED), LPARAM(load_));
        } else if (action == "speed" && IsWindowEnabled(speed_)) {
            SendMessageW(speed_, CB_SETCURSEL, selected, 0);
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kSpeed, CBN_SELCHANGE), LPARAM(speed_));
        } else if (action == "invincible" && IsWindowEnabled(invincible_)) {
            SendMessageW(invincible_, BM_SETCHECK, value == "on" ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kInvincible, BN_CLICKED), LPARAM(invincible_));
        } else if (action == "noclip" && IsWindowEnabled(noclip_)) {
            SendMessageW(noclip_, BM_SETCHECK, value == "on" ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kNoclip, BN_CLICKED), LPARAM(noclip_));
        } else if (action == "resolution" && IsWindowEnabled(resolutionShortcut_)) {
            SendMessageW(resolutionShortcut_, BM_SETCHECK, value == "on" ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kResolutionShortcut, BN_CLICKED), LPARAM(resolutionShortcut_));
        } else if (action == "darkness" && IsWindowEnabled(value == "unlock" ? unlock_ : maxDarkness_)) {
            const auto button = value == "unlock" ? unlock_ : maxDarkness_;
            const auto id = value == "unlock" ? kUnlock : kMaxDarkness;
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), LPARAM(button));
        } else std::puts("[DeveloperTest] Action unavailable; the engine has not reported readiness.");
    }
    printStatus(action);
    return true;
}
