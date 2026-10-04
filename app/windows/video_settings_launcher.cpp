#include "display_settings.h"
#include "runtime/native/fov_settings.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using DarkRecomp::Native::GraphicsSettings;
using DarkRecomp::Native::GameLanguage;

constexpr int kPlayButton = IDOK;
constexpr int kCancelButton = IDCANCEL;
constexpr int kClientWidth = 620;
constexpr int kClientHeight = 650;

enum Row : size_t {
    Brightness,
    FieldOfView,
    Bloom,
    VerticalSync,
    FrameLimit,
    Resolution,
    Display,
    MotionBlur,
    Antialiasing,
    Language,
    TextureFiltering,
    RowCount,
};

constexpr std::array<const wchar_t*, RowCount> kRowNames{
    L"Brightness", L"Field of view", L"Bloom", L"Vertical sync",
    L"Frame limit", L"Resolution", L"Display", L"Motion blur", L"Antialiasing", L"Language", L"Texture filtering",
};

struct Choice {
    std::wstring label;
    double value;
};

struct RowControl {
    HWND combo = nullptr;
    std::vector<Choice> choices;
};

struct Launcher {
    fs::path root;
    fs::path game;
    fs::path preview;
    fs::path settingsPath;
    GraphicsSettings settings;
    float fieldOfView = 0;
    GameLanguage language = GameLanguage::System;
    std::array<RowControl, RowCount> rows;
};

fs::path binaryDirectory() {
    std::vector<wchar_t> module(32768);
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size()));
    if (!length || length >= module.size())
        throw std::runtime_error("Cannot locate the video settings launcher.");
    return fs::path(module.data()).parent_path();
}

fs::path findProjectRoot(fs::path directory) {
    // Releases keep executables in build_native/Release; source builds use the
    // same layout. Also accept a launcher placed directly beside Darkness.
    fs::path incompleteRoot;
    for (unsigned depth = 0; depth < 4; ++depth) {
        if (fs::is_regular_file(directory / L"Darkness/default.xex")) return directory;
        if (incompleteRoot.empty() && fs::is_directory(directory / L"Darkness"))
            incompleteRoot = directory;
        const auto parent = directory.parent_path();
        if (parent == directory || parent.empty()) break;
        directory = parent;
    }
    if (!incompleteRoot.empty()) return incompleteRoot;
    throw std::runtime_error(
        "Cannot find the Darkness folder. Keep this launcher in build_native/Release "
        "and place your game dump in the project's Darkness folder.");
}

void checkGameFiles(const Launcher& launcher) {
    if (!fs::is_regular_file(launcher.preview))
        throw std::runtime_error("DarkRecompPreview.exe is missing. Extract the complete release ZIP.");
    if (!fs::is_regular_file(launcher.preview.parent_path() / L"DarkRecomp.exe"))
        throw std::runtime_error("DarkRecomp.exe is missing. Extract the complete release ZIP.");
    for (const auto* name : {L"default.xex"}) {
        if (!fs::is_regular_file(launcher.game / name))
            throw std::runtime_error("Required game files are missing from Darkness. "
                                     "See START_HERE.txt for the required folder layout.");
    }
    for (const auto* name : {L"Content", L"System"}) {
        if (!fs::is_directory(launcher.game / name))
            throw std::runtime_error("The Content or System folder is missing from Darkness. "
                                     "See START_HERE.txt for the required folder layout.");
    }
}

HWND control(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style,
             int x, int y, int width, int height, int id = 0) {
    HWND window = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                                  x, y, width, height, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    if (!window) throw std::runtime_error("Cannot create a video settings control.");
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return window;
}

void addChoice(RowControl& row, std::wstring label, double value) {
    const LRESULT index = SendMessageW(row.combo, CB_ADDSTRING, 0,
                                      reinterpret_cast<LPARAM>(label.c_str()));
    if (index == CB_ERR || index == CB_ERRSPACE)
        throw std::runtime_error("Cannot populate a video settings control.");
    row.choices.push_back({std::move(label), value});
}

std::wstring savedValueLabel(Row row, double value) {
    wchar_t text[80]{};
    switch (row) {
    case Brightness: std::swprintf(text, 80, L"%u%% (saved)", unsigned(value)); break;
    case FieldOfView: std::swprintf(text, 80, L"%.3f (saved)", value); break;
    case FrameLimit: std::swprintf(text, 80, L"%u FPS (saved)", unsigned(value)); break;
    case Resolution: std::swprintf(text, 80, L"%up (saved)", unsigned(value)); break;
    default: throw std::runtime_error("Unexpected saved video setting.");
    }
    return text;
}

void selectValue(Launcher& launcher, Row row, double value) {
    auto& control = launcher.rows[row];
    size_t selected = control.choices.size();
    for (size_t index = 0; index < control.choices.size(); ++index) {
        if (std::abs(control.choices[index].value - value) < 0.000001) {
            selected = index;
            break;
        }
    }
    if (selected == control.choices.size()) {
        addChoice(control, savedValueLabel(row, value), value);
    }
    if (SendMessageW(control.combo, CB_SETCURSEL, selected, 0) == CB_ERR)
        throw std::runtime_error("Cannot select the saved video setting.");
}

void populateChoices(Launcher& launcher) {
    auto& rows = launcher.rows;
    for (unsigned value = 50; value <= 200; value += 5)
        addChoice(rows[Brightness], std::to_wstring(value) + L"%", value);
    addChoice(rows[FieldOfView], L"Original", 0);
    for (unsigned value = 60; value <= 120; ++value)
        addChoice(rows[FieldOfView], std::to_wstring(value), value);
    addChoice(rows[Bloom], L"Off", 0);
    addChoice(rows[Bloom], L"On", 1);
    addChoice(rows[VerticalSync], L"Off", 0);
    addChoice(rows[VerticalSync], L"On", 1);
    addChoice(rows[FrameLimit], L"Unlimited", 0);
    for (unsigned value : {30u, 60u, 90u, 120u, 144u, 165u, 240u, 360u})
        addChoice(rows[FrameLimit], std::to_wstring(value) + L" FPS", value);
    for (unsigned value : {360u, 480u, 720u, 1080u, 1440u, 2160u})
        addChoice(rows[Resolution], std::to_wstring(value) + L"p", value);
    addChoice(rows[Display], L"Windowed", 0);
    addChoice(rows[Display], L"Borderless fullscreen", 1);
    addChoice(rows[MotionBlur], L"Off", 0);
    addChoice(rows[MotionBlur], L"On", 1);
    addChoice(rows[Antialiasing], L"Off", 0);
    addChoice(rows[Antialiasing], L"FXAA", 1);
    addChoice(rows[TextureFiltering], L"Original", 1);
    for (unsigned level : {2u, 4u, 8u, 16u})
        addChoice(rows[TextureFiltering], std::to_wstring(level) + L"x anisotropic", level);
    for (const auto language : {GameLanguage::System, GameLanguage::English, GameLanguage::German,
                               GameLanguage::French, GameLanguage::Spanish, GameLanguage::Italian})
        addChoice(rows[Language], std::wstring(DarkRecomp::Native::gameLanguageDisplayName(language)),
                  unsigned(language));

    const auto& settings = launcher.settings;
    selectValue(launcher, Brightness, settings.brightnessPercent);
    selectValue(launcher, FieldOfView, launcher.fieldOfView);
    selectValue(launcher, Bloom, settings.bloom);
    selectValue(launcher, VerticalSync, settings.verticalSync);
    selectValue(launcher, FrameLimit, settings.frameRateLimit);
    selectValue(launcher, Resolution, settings.renderHeight);
    selectValue(launcher, Display, settings.fullscreen);
    selectValue(launcher, MotionBlur, settings.motionBlur);
    selectValue(launcher, Antialiasing, settings.antialiasing);
    selectValue(launcher, TextureFiltering, settings.anisotropyLevels);
    selectValue(launcher, Language, unsigned(launcher.language));
}

void createControls(HWND window, Launcher& launcher) {
    control(window, L"STATIC", L"Choose game settings, then click Play.", 0,
            26, 17, 565, 20);
    control(window, L"STATIC", L"Your current settings are loaded automatically.", 0,
            26, 40, 565, 20);
    for (size_t index = 0; index < RowCount; ++index) {
        const int y = 76 + int(index) * 40;
        control(window, L"STATIC", kRowNames[index], 0, 26, y + 4, 164, 22);
        launcher.rows[index].combo = control(window, L"COMBOBOX", L"",
            CBS_DROPDOWNLIST | CBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
            200, y, 380, 280, 2000 + int(index));
    }
    populateChoices(launcher);
    control(window, L"STATIC", L"Gamma calibration uses the in-game Video settings menu.",
            0, 26, 522, 565, 20);
    control(window, L"STATIC",
            L"Settings save when you click Play. Internal resolution takes effect at startup.",
            0, 26, 566, 565, 36);
    control(window, L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP,
            384, 609, 95, 29, kPlayButton);
    control(window, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
            485, 609, 95, 29, kCancelButton);
}

double chosenValue(const Launcher& launcher, Row row) {
    const auto& control = launcher.rows[row];
    const LRESULT index = SendMessageW(control.combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || size_t(index) >= control.choices.size())
        throw std::runtime_error("Select a value for every video setting.");
    return control.choices[size_t(index)].value;
}

void saveAndLaunch(HWND window, Launcher& launcher) {
    GraphicsSettings settings = launcher.settings;
    settings.brightnessPercent = unsigned(chosenValue(launcher, Brightness));
    const float fov = float(chosenValue(launcher, FieldOfView));
    settings.bloom = chosenValue(launcher, Bloom) != 0;
    settings.verticalSync = chosenValue(launcher, VerticalSync) != 0;
    settings.frameRateLimit = unsigned(chosenValue(launcher, FrameLimit));
    settings.renderHeight = unsigned(chosenValue(launcher, Resolution));
    settings.fullscreen = chosenValue(launcher, Display) != 0;
    settings.motionBlur = chosenValue(launcher, MotionBlur) != 0;
    settings.antialiasing = chosenValue(launcher, Antialiasing) != 0;
    settings.anisotropyLevels = unsigned(chosenValue(launcher, TextureFiltering));
    const auto language = GameLanguage(unsigned(chosenValue(launcher, Language)));

    if (!DarkRecomp::saveDisplaySettings(launcher.settingsPath, fov, settings, language))
        throw std::runtime_error("Could not save DarkRecomp.settings.ini. "
                                 "Check that the game folder is writable, then retry.");

    std::wstring command = L"\"" + launcher.preview.wstring() + L"\" --sound";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(launcher.preview.c_str(), command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, launcher.root.c_str(), &startup, &process)) {
        throw std::runtime_error("Settings were saved, but the game could not start "
                                 "(Windows error " + std::to_string(GetLastError()) + ").");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DestroyWindow(window);
}

LRESULT CALLBACK launcherWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* launcher = reinterpret_cast<Launcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == kPlayButton && HIWORD(wParam) == BN_CLICKED && launcher) {
            try { saveAndLaunch(window, *launcher); }
            catch (const std::exception& error) {
                MessageBoxA(window, error.what(), "Settings launch failed", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(wParam) == kCancelButton && HIWORD(wParam) == BN_CLICKED) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        Launcher launcher;
        const auto binary = binaryDirectory();
        launcher.root = findProjectRoot(binary);
        launcher.game = launcher.root / L"Darkness";
        launcher.preview = binary / L"DarkRecompPreview.exe";
        launcher.settingsPath = launcher.root / L"DarkRecomp.settings.ini";
        checkGameFiles(launcher);
        launcher.settings = DarkRecomp::loadGraphicsSettings(launcher.settingsPath);
        launcher.fieldOfView = DarkRecomp::loadFieldOfView(launcher.settingsPath);
        launcher.language = DarkRecomp::Native::loadGameLanguage(launcher.settingsPath);

        constexpr auto className = L"DarkRecompVideoSettingsLauncher";
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = launcherWindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = className;
        if (!RegisterClassExW(&windowClass))
            throw std::runtime_error("Cannot register the video settings window.");
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        constexpr DWORD exStyle = WS_EX_CONTROLPARENT;
        RECT bounds{0, 0, kClientWidth, kClientHeight};
        if (!AdjustWindowRectEx(&bounds, style, FALSE, exStyle))
            throw std::runtime_error("Cannot size the video settings window.");
        HWND window = CreateWindowExW(exStyle, className, L"The Darkness - Settings",
            style, CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, &launcher);
        if (!window) throw std::runtime_error("Cannot open the video settings window.");
        createControls(window, launcher);
        ShowWindow(window, show);
        UpdateWindow(window);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return int(message.wParam);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "Video settings launcher failed", MB_OK | MB_ICONERROR);
        return 1;
    }
}
