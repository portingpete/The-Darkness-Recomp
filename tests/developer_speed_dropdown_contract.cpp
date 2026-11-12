#include "app/windows/developer_tools_window.h"
#include "app/windows/native_mouse.h"
#include "runtime/native/developer_tools.h"
#include <array>
#include <commctrl.h>
#include <cmath>
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

// Exercise real Win32 slider behavior without showing a window on the user's
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
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"Speed slider contract",
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
    LRESULT selected() const { return SendMessageW(speed(), TBM_GETPOS, 0, 0); }
    void refresh() { panel.refresh(); }
    void slide(int position, unsigned notification = TB_THUMBTRACK) {
        SendMessageW(speed(), TBM_SETPOS, TRUE, position);
        SendMessageW(panel.window_, WM_HSCROLL, MAKEWPARAM(notification, position), LPARAM(speed()));
    }
    void pump(unsigned milliseconds) {
        const auto end = GetTickCount64() + milliseconds;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.hwnd == panel.window_ && message.message == WM_TIMER) ++timerTicks;
                if (!panel.handleMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < end);
    }
};

int main() {
    try {
        PrivateDesktop desktop;
        DeveloperToolsWindowTestFixture fixture;
        wchar_t type[64]{};
        GetClassNameW(fixture.speed(), type, 64);
        require(std::wstring_view(type) == TRACKBAR_CLASSW, "Speed control is not a native slider");
        require(fixture.selected() == 300 && snapshot.playerSpeed == 1, "Default speed is not centered");
        require(SendMessageW(fixture.speed(), TBM_GETTIC, 0, 0) == 300, "Default center line is missing");
        auto before = speedRequests;
        fixture.slide(600);
        require(snapshot.playerSpeed == 4 && speedRequests == before + 1, "Drag did not apply maximum speed");
        fixture.pump(800);
        require(fixture.timerTicks >= 3 && fixture.selected() == 600 && snapshot.playerSpeed == 4,
                "Refresh interrupted the active drag");
        fixture.slide(600, TB_ENDTRACK);
        require(speedRequests == before + 1, "Drag completion duplicated the request");
        fixture.slide(0, TB_THUMBPOSITION);
        require(snapshot.playerSpeed == .25f, "Slider minimum changed");
        SendMessageW(fixture.speed(), WM_KEYDOWN, VK_RIGHT, 0);
        SendMessageW(fixture.speed(), WM_KEYUP, VK_RIGHT, 0);
        require(snapshot.playerSpeed > .25f && snapshot.playerSpeed < .26f && fixture.selected() == 1,
                "Keyboard adjustment did not use the slider callback");
        require(fixture.panel.handleTestCommand("dev speed 0.5") && snapshot.playerSpeed == .5f && fixture.selected() == 100,
                "Existing diagnostic preset did not use the slider");
        require(fixture.panel.handleTestCommand("dev speed 1.1") && std::abs(snapshot.playerSpeed - 1.1f) < .001f,
                "Slider diagnostics still require the retired dropdown presets");
        require(!fixture.panel.handleTestCommand("dev speed nan") && !fixture.panel.handleTestCommand("dev speed 5"),
                "Invalid slider input was accepted");
        require(fixture.panel.handleTestCommand("dev defaults") && snapshot.playerSpeed == 1 && fixture.selected() == 300,
                "Defaults did not restore the marked center");
        snapshot.hasActivePlayer = false; fixture.refresh(); before = speedRequests;
        require(!IsWindowEnabled(fixture.speed()), "Speed slider enabled without an active player");
        fixture.panel.handleTestCommand("dev speed 2");
        require(speedRequests == before, "Unavailable control applied speed");
        require(!toolsVisible, "Hidden fixture marked developer tools visible");
        std::puts("Developer speed slider: native trackbar, centered default line, drag/refresh, keyboard, diagnostics and availability passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DeveloperSpeedSliderContract: %s\n", error.what()); return 1;
    }
}
