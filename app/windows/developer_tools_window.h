#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <functional>

class NativeMouseWindow;

// The display thread owns the panel and its controls. All game actions cross
// the value-only developer-tools bridge; this class never accesses guest data.
class DeveloperToolsWindow {
public:
    DeveloperToolsWindow(HWND owner, NativeMouseWindow& mouse,
                         std::function<void(bool)> resolutionShortcut = {});
    ~DeveloperToolsWindow();
    DeveloperToolsWindow(const DeveloperToolsWindow&) = delete;
    DeveloperToolsWindow& operator=(const DeveloperToolsWindow&) = delete;

    bool isOpen() const { return window_ != nullptr; }
    bool handleMessage(MSG& message);
    void toggle();
    void close();
    // Opt-in own-process diagnostics exercise the same controls and callbacks.
    // False means an invalid command; valid but unavailable actions log status.
    bool handleTestCommand(std::string_view command);

private:
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    bool open();
    void createControls();
    void refresh();
    void loadMission();
    void selectSpeed();
    void selectInvincibility();
    void selectNoclip();
    void selectResolutionShortcut();
    void unlockDarkness();
    void maxDarkness();
    void restoreDefaults();
    void releaseInputGate();
    void printStatus(std::string_view action) const;
    bool capturePanel(std::string_view path);

    HWND owner_{};
    NativeMouseWindow& mouse_;
    HWND window_{}, mission_{}, load_{}, speed_{}, invincible_{}, noclip_{}, unlock_{}, maxDarkness_{},
         resolutionShortcut_{}, defaults_{}, status_{};
    std::function<void(bool)> setResolutionShortcut_;
    bool resolutionShortcutEnabled_ = false;
    bool inputGate_ = false;
    std::wstring displayedStatus_;
};
