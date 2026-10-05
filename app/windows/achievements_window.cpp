#include "achievements_window.h"
#include "native_mouse.h"
#include "runtime/native/input.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace DarkRecomp::Native;

namespace {
constexpr wchar_t kWindowClass[] = L"DarkRecompAchievementsWindow";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kAchievements = 1101;

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!size) return L"Text unavailable.";
    std::wstring converted(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), converted.data(), size);
    return converted;
}

HWND control(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
             int x, int y, int width, int height, int id = 0, DWORD extended = 0) {
    const HWND result = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    if (!result) throw std::runtime_error("Cannot create achievements control");
    SendMessageW(result, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return result;
}

struct Totals {
    size_t unlocked = 0;
    uint64_t earned = 0, possible = 0;
};

Totals totals(const Achievements::Snapshot& snapshot) {
    Totals result;
    for (const auto& entry : snapshot.entries) {
        result.possible += entry.gamerscore;
        if (entry.unlockedAt) {
            ++result.unlocked;
            result.earned += entry.gamerscore;
        }
    }
    return result;
}
}

AchievementsWindow::AchievementsWindow(HWND owner, NativeMouseWindow& mouse) : owner_(owner), mouse_(mouse) {}
AchievementsWindow::~AchievementsWindow() { close(); releaseInputGate(); }

void AchievementsWindow::update() {
    if (Achievements::consumeShowRequest()) open();
}

bool AchievementsWindow::handleMessage(MSG& message) {
    if (!window_ || (message.hwnd != window_ && !IsChild(window_, message.hwnd))) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        close();
        return true;
    }
    return IsDialogMessageW(window_, &message) != FALSE;
}

bool AchievementsWindow::open() {
    if (window_) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        SetFocus(list_);
        return true;
    }
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
        std::fputs("[Achievements] Cannot register viewer window.\n", stderr);
        return false;
    }
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD extended = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW;
    RECT size{0, 0, 640, 524};
    AdjustWindowRectEx(&size, style, FALSE, extended);
    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    const int width = size.right - size.left, height = size.bottom - size.top;
    int x = ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2;
    int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        x = (std::max)(monitor.rcWork.left, (std::min)(LONG(x), monitor.rcWork.right - width));
        y = (std::max)(monitor.rcWork.top, (std::min)(LONG(y), monitor.rcWork.bottom - height));
    }
    const HWND created = CreateWindowExW(extended, kWindowClass, L"THE DARKNESS - ACHIEVEMENTS",
        style, x, y, width, height, owner_, nullptr, windowClass.hInstance, this);
    if (!created) {
        releaseInputGate();
        std::fputs("[Achievements] Cannot create viewer window.\n", stderr);
        return false;
    }
    Achievements::setViewerOpen(true);
    refresh();
    SetTimer(window_, kRefreshTimer, 500, nullptr);
    ShowWindow(window_, SW_SHOW);
    SetFocus(list_);
    std::puts("[Achievements] Viewer opened; game input suspended. Escape closes it.");
    return true;
}

void AchievementsWindow::close() {
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    else releaseInputGate();
}

void AchievementsWindow::releaseInputGate() {
    if (!inputGate_) return;
    inputGate_ = false;
    Achievements::setViewerOpen(false);
    nativeInput().setSettingsOpen(false);
}

void AchievementsWindow::createControls() {
    summary_ = control(window_, L"STATIC", L"", SS_LEFT, 16, 17, 608, 20);
    localStatus_ = control(window_, L"STATIC", L"", SS_LEFT, 16, 42, 608, 38);
    control(window_, L"STATIC", L"STATUS / GAMERSCORE / ACHIEVEMENT", SS_LEFT, 16, 89, 608, 18);
    list_ = control(window_, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 16, 112, 608, 244, kAchievements, WS_EX_CLIENTEDGE);
    control(window_, L"STATIC", L"DESCRIPTION", SS_LEFT, 16, 369, 608, 18);
    description_ = control(window_, L"EDIT", L"", ES_LEFT | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 16, 391, 608, 79, 0, WS_EX_CLIENTEDGE);
    control(window_, L"STATIC", L"Select an achievement for its details.", SS_LEFT, 16, 492, 490, 18);
    control(window_, L"BUTTON", L"CLOSE", BS_PUSHBUTTON | WS_TABSTOP, 546, 485, 78, 27, IDCANCEL);
}

void AchievementsWindow::refresh() {
    if (!window_ || !list_) return;
    const auto previousSelection = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    uint32_t selectedId = 0;
    bool hasSelection = previousSelection >= 0 && size_t(previousSelection) < snapshot_.entries.size();
    if (hasSelection) selectedId = snapshot_.entries[size_t(previousSelection)].id;
    const auto topIndex = SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
    auto latest = Achievements::snapshot();
    const auto progress = totals(latest);
    std::wstring summary = std::to_wstring(progress.unlocked) + L" / " +
        std::to_wstring(latest.entries.size()) + L" unlocked     " + std::to_wstring(progress.earned) +
        L" / " + std::to_wstring(progress.possible) + L" gamerscore";
    std::wstring localStatus = latest.error.empty() ? L"Saved locally on this PC." :
        L"Local achievements: " + wide(latest.error);
    if (summary != displayedSummary_) {
        displayedSummary_ = std::move(summary);
        SetWindowTextW(summary_, displayedSummary_.c_str());
    }
    if (localStatus != displayedLocalStatus_) {
        displayedLocalStatus_ = std::move(localStatus);
        SetWindowTextW(localStatus_, displayedLocalStatus_.c_str());
    }
    std::vector<std::wstring> rows;
    rows.reserve(latest.entries.size());
    for (const auto& entry : latest.entries) {
        rows.push_back(std::wstring(entry.unlockedAt ? L"UNLOCKED" : L"LOCKED") + L"  /  " +
            std::to_wstring(entry.gamerscore) + L" G  /  " + wide(entry.name));
    }
    snapshot_ = std::move(latest);
    if (rows != displayedRows_) {
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        int extent = 0;
        HDC device = GetDC(list_);
        const HFONT font = reinterpret_cast<HFONT>(SendMessageW(list_, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = device && font ? SelectObject(device, font) : nullptr;
        for (const auto& row : rows) {
            const auto added = SendMessageW(list_, LB_ADDSTRING, 0, LPARAM(row.c_str()));
            if (added == LB_ERR || added == LB_ERRSPACE) {
                if (oldFont) SelectObject(device, oldFont);
                if (device) ReleaseDC(list_, device);
                SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
                throw std::runtime_error("Cannot populate achievements list");
            }
            SIZE size{};
            if (device && GetTextExtentPoint32W(device, row.c_str(), int(row.size()), &size))
                extent = (std::max)(extent, int(size.cx) + 12);
        }
        if (oldFont) SelectObject(device, oldFont);
        if (device) ReleaseDC(list_, device);
        SendMessageW(list_, LB_SETHORIZONTALEXTENT, extent, 0);
        size_t selectedIndex = 0;
        if (hasSelection) {
            const auto selected = std::find_if(snapshot_.entries.begin(), snapshot_.entries.end(),
                [selectedId](const auto& entry) { return entry.id == selectedId; });
            if (selected != snapshot_.entries.end()) selectedIndex = size_t(selected - snapshot_.entries.begin());
        }
        if (!rows.empty()) {
            SendMessageW(list_, LB_SETCURSEL, selectedIndex, 0);
            if (topIndex >= 0) SendMessageW(list_, LB_SETTOPINDEX, topIndex, 0);
        }
        displayedRows_ = std::move(rows);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
    }
    selectAchievement();
}

void AchievementsWindow::selectAchievement() {
    const LRESULT selected = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    std::wstring description = L"No achievements are available.";
    if (selected >= 0 && size_t(selected) < snapshot_.entries.size()) {
        const auto& entry = snapshot_.entries[size_t(selected)];
        const std::string& text = !entry.unlockedAt && !entry.lockedDescription.empty() ?
            entry.lockedDescription : entry.description;
        description = wide(entry.name) + L"\r\n" + (entry.unlockedAt ? L"Unlocked" : L"Locked") +
            L" - " + std::to_wstring(entry.gamerscore) + L" gamerscore\r\n\r\n" + wide(text);
    }
    if (description != displayedDescription_) {
        displayedDescription_ = std::move(description);
        SetWindowTextW(description_, displayedDescription_.c_str());
    }
}

LRESULT CALLBACK AchievementsWindow::windowProc(HWND window, UINT message, WPARAM key, LPARAM detail) {
    auto* self = reinterpret_cast<AchievementsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<AchievementsWindow*>(reinterpret_cast<CREATESTRUCTW*>(detail)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(self));
    }
    if (self) {
        try { return self->message(message, key, detail); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "[Achievements] %s\n", error.what());
            if (message == WM_CREATE) return -1;
        }
    }
    return DefWindowProcW(window, message, key, detail);
}

LRESULT AchievementsWindow::message(UINT message, WPARAM key, LPARAM detail) {
    if (message == WM_CREATE) { createControls(); return 0; }
    if (message == WM_TIMER && key == kRefreshTimer) { refresh(); return 0; }
    if (message == WM_COMMAND) {
        const int id = LOWORD(key), notification = HIWORD(key);
        if (id == IDCANCEL && notification == BN_CLICKED) close();
        else if (id == kAchievements && notification == LBN_SELCHANGE) selectAchievement();
        return 0;
    }
    if (message == WM_CLOSE) { close(); return 0; }
    if (message == WM_NCDESTROY) {
        const HWND closed = window_;
        const HWND foreground = GetForegroundWindow();
        const bool restoreFocus = foreground == closed || foreground == owner_;
        KillTimer(closed, kRefreshTimer);
        SetWindowLongPtrW(closed, GWLP_USERDATA, 0);
        window_ = summary_ = localStatus_ = list_ = description_ = nullptr;
        snapshot_ = {};
        displayedRows_.clear();
        displayedSummary_.clear();
        displayedLocalStatus_.clear();
        displayedDescription_.clear();
        releaseInputGate();
        if (restoreFocus && IsWindow(owner_)) SetFocus(owner_);
        std::puts("[Achievements] Viewer closed; click the game to capture the mouse.");
        return DefWindowProcW(closed, message, key, detail);
    }
    return DefWindowProcW(window_, message, key, detail);
}

void AchievementsWindow::printStatus(std::string_view action) const {
    const auto state = Achievements::snapshot();
    const auto progress = totals(state);
    const LRESULT selected = list_ ? SendMessageW(list_, LB_GETCURSEL, 0, 0) : LB_ERR;
    const auto selectedId = selected >= 0 && size_t(selected) < snapshot_.entries.size() ?
        snapshot_.entries[size_t(selected)].id : 0;
    std::printf("[AchievementsTest] action=%.*s open=%u unlocked=%zu count=%zu earned=%llu total=%llu selected=%u error=%s\n",
        int(action.size()), action.data(), unsigned(isOpen()), progress.unlocked, state.entries.size(),
        static_cast<unsigned long long>(progress.earned), static_cast<unsigned long long>(progress.possible),
        selectedId, state.error.c_str());
}

bool AchievementsWindow::capturePanel(std::string_view path) {
    if (!window_ || !IsWindow(window_)) {
        std::fputs("[AchievementsTest] Viewer capture unavailable while closed.\n", stderr);
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
    // Print only this process's owned achievements viewer into a bitmap.
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

bool AchievementsWindow::handleTestCommand(std::string_view command) {
    std::istringstream fields{std::string(command)};
    std::string prefix, action, value, extra;
    if (!(fields >> prefix >> action) || prefix != "achievements") return false;
    if (action == "open" || action == "close" || action == "status") {
        if (fields >> extra) return false;
        if (action == "open") open();
        else if (action == "close") close();
        printStatus(action);
        return true;
    }
    // Windows paths use backslashes literally, including inside quotes.
    if (!(fields >> std::quoted(value, '"', '\0')) || fields >> extra) return false;
    if (action == "capture") {
        if (!std::filesystem::path(wide(value)).is_absolute()) return false;
        const bool saved = capturePanel(value);
        std::printf("[AchievementsTest] Viewer capture saved=%u path=%s\n", unsigned(saved), value.c_str());
        printStatus(action);
        return true;
    }
    if (action == "select") {
        uint32_t id = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
        if (!window_) {
            std::puts("[AchievementsTest] Selection unavailable while closed; use 'achievements open' first.");
        } else {
            refresh();
            const auto selected = std::find_if(snapshot_.entries.begin(), snapshot_.entries.end(),
                [id](const auto& entry) { return entry.id == id; });
            if (selected == snapshot_.entries.end()) return false;
            const auto index = size_t(selected - snapshot_.entries.begin());
            SendMessageW(list_, LB_SETCURSEL, index, 0);
            SendMessageW(window_, WM_COMMAND, MAKEWPARAM(kAchievements, LBN_SELCHANGE), LPARAM(list_));
        }
        printStatus(action);
        return true;
    }
    return false;
}
