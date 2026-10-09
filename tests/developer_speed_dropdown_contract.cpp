#include "app/windows/developer_tools_window.h"
#include "app/windows/native_mouse.h"
#include "runtime/native/developer_tools.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace DarkRecomp::Native;

namespace {
DeveloperSnapshot snapshot{true, true};
unsigned speedRequests = 0;
bool toolsVisible = false;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Exercise real Win32 popup behavior without showing a window on the user's
// desktop, taking their foreground focus, or injecting any global input.
struct PrivateDesktop {
    HDESK previous = GetThreadDesktop(GetCurrentThreadId());
    HDESK handle = nullptr;
    PrivateDesktop() {
        const auto name = L"DarkRecompSpeedTest-" + std::to_wstring(GetCurrentProcessId());
        handle = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
        require(handle != nullptr, "Cannot create private test desktop");
        if (!SetThreadDesktop(handle)) {
            CloseDesktop(handle);
            handle = nullptr;
            throw std::runtime_error("Cannot attach private test desktop");
        }
    }
    ~PrivateDesktop() {
        SetThreadDesktop(previous);
        if (handle) CloseDesktop(handle);
    }
};
}

// The GUI calls only the value bridge. This fixture supplies player readiness
// and records speed requests; no guest engine, game assets, or saves are used.
namespace DarkRecomp::Native {
NativeInput::NativeInput(ControllerApi api, bool) : api_(api), bindings_{} {}
NativeInput::~NativeInput() = default;
void NativeInput::setSettingsOpen(bool) {}
void NativeInput::setMouseLookEnabled(bool, bool) {}
void NativeInput::cancelGameplayMouseCaptureRequest() {}
NativeInput& nativeInput() { static NativeInput input; return input; }

DeveloperSnapshot developerSnapshot() { return snapshot; }
std::span<const DeveloperMission> developerMissions() noexcept {
    static constexpr std::array<DeveloperMission, 1> missions{{{"fixture", "Fixture"}}};
    return missions;
}
void setDeveloperToolsVisible(bool visible) { toolsVisible = visible; }
bool requestDeveloperMission(std::string_view) { return false; }
bool requestDeveloperSpeed(float speed) {
    snapshot.playerSpeed = speed;
    ++snapshot.revision;
    ++speedRequests;
    return true;
}
void requestDeveloperInvincibility(bool enabled) { snapshot.invincible = enabled; }
void requestDeveloperNoclip(bool enabled) { snapshot.noclip = enabled; }
void requestDeveloperUnlockDarkness() {}
void requestDeveloperMaxDarkness() {}
}

struct DeveloperToolsWindowTestFixture {
    HWND owner = nullptr;
    NativeMouseWindow mouse;
    DeveloperToolsWindow panel;
    unsigned timerTicks = 0;

    static HWND createOwner() {
        const auto window = CreateWindowExW(0, L"STATIC", L"Developer speed fixture", WS_POPUP,
            0, 0, 450, 452, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Cannot create fixture owner");
        return window;
    }

    DeveloperToolsWindowTestFixture() : owner(createOwner()), mouse(owner), panel(owner, mouse) {
        WNDCLASSW type{};
        type.lpfnWndProc = DeveloperToolsWindow::windowProc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"DarkRecompDeveloperSpeedTest";
        require(RegisterClassW(&type) != 0, "Cannot register fixture panel");
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"Speed dropdown contract",
            WS_POPUP, 0, 0, 450, 452, owner, nullptr, type.hInstance, &panel);
        require(window != nullptr, "Cannot create production developer panel");
        panel.refresh();
        // The production panel uses this timer ID and interval.
        require(SetTimer(window, 1, 250, nullptr) != 0, "Cannot start panel refresh timer");
    }

    ~DeveloperToolsWindowTestFixture() {
        panel.close();
        if (owner) DestroyWindow(owner);
        UnregisterClassW(L"DarkRecompDeveloperSpeedTest", GetModuleHandleW(nullptr));
    }

    HWND speed() const { return panel.speed_; }
    LRESULT selected() const { return SendMessageW(speed(), CB_GETCURSEL, 0, 0); }
    bool dropped() const { return SendMessageW(speed(), CB_GETDROPPEDSTATE, 0, 0) != 0; }

    void pump(unsigned milliseconds) {
        const auto end = GetTickCount64() + milliseconds;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.hwnd == panel.window_ && message.message == WM_TIMER && message.wParam == 1)
                    ++timerTicks;
                if (!panel.handleMessage(message)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < end);
    }

    HWND openList() {
        SendMessageW(speed(), CB_SHOWDROPDOWN, TRUE, 0);
        require(dropped(), "Native combo did not open");
        COMBOBOXINFO combo{sizeof(combo)};
        require(GetComboBoxInfo(speed(), &combo) != FALSE && IsWindow(combo.hwndList),
            "Native combo list unavailable");
        return combo.hwndList;
    }

    static LPARAM rowPoint(HWND list, int row) {
        RECT item{};
        require(SendMessageW(list, LB_GETITEMRECT, row, LPARAM(&item)) != LB_ERR,
            "Cannot locate native popup row");
        return MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
    }

    void hover(HWND list, int row) {
        SendMessageW(list, WM_MOUSEMOVE, 0, rowPoint(list, row));
        require(SendMessageW(list, LB_GETCURSEL, 0, 0) == row,
            "Mouse hover did not highlight requested native row");
    }

    void click(HWND list, int row) {
        const auto point = rowPoint(list, row);
        SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(list, WM_LBUTTONUP, 0, point);
        require(!dropped(), "Mouse acceptance did not close popup");
    }

    void escape() {
        MSG message{};
        message.hwnd = speed(); message.message = WM_KEYDOWN; message.wParam = VK_ESCAPE;
        require(!panel.handleMessage(message), "Escape closed panel instead of canceling popup");
        DispatchMessageW(&message);
        require(panel.isOpen() && !dropped(), "Escape did not cancel only the popup");
    }
};

namespace {
void mouseSelectionAcrossRefresh(DeveloperToolsWindowTestFixture& fixture) {
    require(fixture.selected() == 3 && snapshot.playerSpeed == 1,
        "Fixture did not begin at normal speed");
    for (const auto [row, value] : std::array<std::pair<int, float>, 2>{{{6, 2.f}, {8, 4.f}}}) {
        const auto requests = speedRequests;
        const auto ticks = fixture.timerTicks;
        const auto list = fixture.openList();
        fixture.hover(list, row);
        fixture.pump(800);
        require(fixture.timerTicks >= ticks + 3, "Did not exercise repeated production refresh timer ticks");
        require(SendMessageW(list, LB_GETCURSEL, 0, 0) == row,
            "Refresh timer reset mouse-highlighted speed to the current game speed");
        require(speedRequests == requests, "Hover applied an unaccepted speed");
        fixture.click(list, row);
        require(snapshot.playerSpeed == value && fixture.selected() == row && speedRequests == requests + 1,
            "Mouse acceptance failed to apply selected speed exactly once");
        fixture.pump(300);
        require(snapshot.playerSpeed == value && fixture.selected() == row,
            "Accepted speed did not survive the next refresh");
    }
}

void canceledSelection(DeveloperToolsWindowTestFixture& fixture) {
    const auto requests = speedRequests;
    const auto list = fixture.openList();
    fixture.hover(list, 3);
    fixture.pump(300);
    fixture.escape();
    fixture.pump(300);
    require(snapshot.playerSpeed == 4 && fixture.selected() == 8 && speedRequests == requests,
        "Cancel changed speed or retained an unaccepted row");
}

void keyboardAndDiagnostics(DeveloperToolsWindowTestFixture& fixture) {
    const auto requests = speedRequests;
    SendMessageW(fixture.speed(), WM_KEYDOWN, VK_UP, 0);
    SendMessageW(fixture.speed(), WM_KEYUP, VK_UP, 0);
    require(snapshot.playerSpeed == 3 && fixture.selected() == 7 && speedRequests == requests + 1,
        "Closed-dropdown keyboard selection stopped applying immediately");
    fixture.pump(300);
    require(snapshot.playerSpeed == 3 && fixture.selected() == 7, "Keyboard speed did not survive refresh");

    fixture.openList();
    SendMessageW(fixture.speed(), WM_KEYDOWN, VK_UP, 0);
    SendMessageW(fixture.speed(), WM_KEYUP, VK_UP, 0);
    fixture.pump(300);
    require(snapshot.playerSpeed == 3 && fixture.selected() == 6 && speedRequests == requests + 1,
        "Open-dropdown keyboard preview applied early or reset during refresh");
    SendMessageW(fixture.speed(), WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(fixture.speed(), WM_KEYUP, VK_RETURN, 0);
    require(!fixture.dropped() && snapshot.playerSpeed == 2 && fixture.selected() == 6 &&
            speedRequests == requests + 2, "Open-dropdown keyboard acceptance failed");

    const auto list = fixture.openList();
    fixture.hover(list, 0);
    require(fixture.panel.handleTestCommand("dev speed 0.5"), "Existing speed diagnostic rejected valid command");
    require(!fixture.dropped() && snapshot.playerSpeed == .5f && fixture.selected() == 1,
        "Scripted speed change did not close pending popup and apply");

    snapshot.hasActivePlayer = false;
    const auto unavailableRequests = speedRequests;
    require(fixture.panel.handleTestCommand("dev speed 2"), "Unavailable diagnostic command rejected");
    require(!IsWindowEnabled(fixture.speed()) && speedRequests == unavailableRequests,
        "Unavailable player control applied a speed");
    snapshot.hasActivePlayer = true;
    fixture.pump(300);
    require(IsWindowEnabled(fixture.speed()) && fixture.selected() == 1,
        "Player readiness refresh failed to restore accepted speed");
}
}

int main() {
    try {
        PrivateDesktop desktop;
        DeveloperToolsWindowTestFixture fixture;
        mouseSelectionAcrossRefresh(fixture);
        canceledSelection(fixture);
        keyboardAndDiagnostics(fixture);
        require(!toolsVisible, "Hidden fixture marked game developer tools visible");
        std::puts("Developer speed dropdown: native mouse hover across refresh, acceptance, cancellation, closed/open keyboard selection, diagnostics and availability verified.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DeveloperSpeedDropdownContract: %s\n", error.what());
        return 1;
    }
}
