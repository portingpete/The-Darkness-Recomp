#pragma once
#include <windows.h>
#include "runtime/native/achievements.h"
#include <string>
#include <string_view>
#include <vector>

class NativeMouseWindow;

// The display thread owns this viewer and all of its controls. The achievement
// bridge supplies value-only snapshots; the viewer never accesses guest memory.
class AchievementsWindow {
public:
    AchievementsWindow(HWND owner, NativeMouseWindow& mouse);
    ~AchievementsWindow();
    AchievementsWindow(const AchievementsWindow&) = delete;
    AchievementsWindow& operator=(const AchievementsWindow&) = delete;

    void update();
    bool handleMessage(MSG& message);
    void close();
    bool isOpen() const { return window_ != nullptr; }
    // Opt-in diagnostics use this process's viewer, never another application's
    // window. Supported commands: achievements open/close/status/select/capture.
    bool handleTestCommand(std::string_view command);

private:
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    bool open();
    void createControls();
    void refresh();
    void selectAchievement();
    void releaseInputGate();
    void printStatus(std::string_view action) const;
    bool capturePanel(std::string_view path);

    HWND owner_{};
    NativeMouseWindow& mouse_;
    HWND window_{}, summary_{}, localStatus_{}, list_{}, description_{};
    bool inputGate_ = false;
    DarkRecomp::Native::Achievements::Snapshot snapshot_;
    std::vector<std::wstring> displayedRows_;
    std::wstring displayedSummary_, displayedLocalStatus_, displayedDescription_;
};
