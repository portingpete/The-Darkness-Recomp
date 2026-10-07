#include "app/windows/keyboard_settings.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"DarkRecomp-keyboard-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    struct Cleanup {
        std::filesystem::path path; bool owned = false;
        ~Cleanup() { if (owned) { std::error_code error; std::filesystem::remove(path, error); } }
    } cleanup{path};
    try {
        check(!std::filesystem::exists(path), "isolated fixture already exists");
        cleanup.owned = true;
        const auto defaults = defaultKeyboardBindings();
        check(validKeyboardBindings(defaults) && loadKeyboardBindings(path) == defaults, "default selection invalid");
        check(defaults.keys[size_t(KeyboardAction::Crouch)] == std::array<uint16_t, 2>{VK_CONTROL, 'C'},
              "default crouch alternatives lost");
        check(defaults.keys[size_t(KeyboardAction::FireRight)] == std::array<uint16_t, 2>{'X', VK_LBUTTON} &&
              defaults.keys[size_t(KeyboardAction::FireLeft)] == std::array<uint16_t, 2>{'Z', VK_RBUTTON} &&
              defaults.keys[size_t(KeyboardAction::Zoom)] == std::array<uint16_t, 2>{VK_SHIFT, VK_MBUTTON},
              "default mouse alternatives lost");
        const std::array<std::pair<unsigned, const char*>, 5> mouseKeys{{
            {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"},
            {VK_XBUTTON1, "M4"}, {VK_XBUTTON2, "M5"}
        }};
        for (const auto [key, prompt] : mouseKeys) {
            auto mouseBinding = defaults;
            check(assignKeyboardKey(mouseBinding, KeyboardAction::Jump, 0, key) &&
                  mouseBinding.keys[size_t(KeyboardAction::Jump)][0] == key &&
                  validKeyboardBindings(mouseBinding), "mouse button assignment rejected");
            check(!keyboardKeyName(key).empty() && keyboardKeyPrompt(key) == prompt,
                  "mouse button label unavailable");
        }
        auto mouseConflict = defaults;
        check(assignKeyboardKey(mouseConflict, KeyboardAction::Jump, 0, VK_LBUTTON) &&
              mouseConflict.keys[size_t(KeyboardAction::FireRight)][1] == VK_SPACE,
              "duplicate mouse button did not swap the displaced binding");
        auto selected = defaults;
        check(assignKeyboardKey(selected, KeyboardAction::MoveForward, 0, VK_UP), "arrow movement assignment rejected");
        check(assignKeyboardKey(selected, KeyboardAction::Jump, 0, 'E') &&
              selected.keys[size_t(KeyboardAction::Use)][0] == VK_SPACE &&
              selected.keys[size_t(KeyboardAction::Jump)][0] == 'E', "duplicate action did not swap assignments");
        check(assignKeyboardKey(selected, KeyboardAction::Reload, 1, VK_OEM_1), "punctuation assignment rejected");
        check(assignKeyboardKey(selected, KeyboardAction::Crouch, 1, 0), "unbinding secondary rejected");
        check(assignKeyboardKey(selected, KeyboardAction::Crouch, 1, VK_MENU) &&
              keyboardKeyName(VK_MENU) == L"ALT" && keyboardKeyPrompt(VK_MENU) == "Alt",
              "Alt assignment or labels rejected");
        check(assignKeyboardKey(selected, KeyboardAction::Use, 1, VK_LBUTTON) &&
              assignKeyboardKey(selected, KeyboardAction::LookUp, 1, VK_XBUTTON1) &&
              assignKeyboardKey(selected, KeyboardAction::LookDown, 1, VK_XBUTTON2),
              "mouse persistence selection rejected");
        for (const auto key : {VK_ESCAPE,VK_F1,VK_F2,VK_F5,VK_F6,VK_F8,VK_LWIN,VK_RWIN,256}) {
            const auto before = selected;
            check(!assignKeyboardKey(selected, KeyboardAction::Jump, 0, unsigned(key)) && selected == before,
                  "reserved key assignment changed controls");
        }
        check(!assignKeyboardKey(selected, KeyboardAction::Count, 0, 'P') &&
              !assignKeyboardKey(selected, KeyboardAction::Jump, 2, 'P'), "invalid action or slot accepted");
        check(WritePrivateProfileStringW(L"Display", L"FieldOfView", L"93.125", path.c_str()) &&
              WritePrivateProfileStringW(L"Other", L"Preserve", L"yes", path.c_str()), "cannot seed unrelated settings");
        check(saveKeyboardBindings(path, selected) && loadKeyboardBindings(path) == selected, "keyboard round trip failed");
        check(GetPrivateProfileIntW(L"Keyboard", L"BindingsVersion", 0, path.c_str()) == 2,
              "saved bindings did not mark mouse-aware configuration");
        wchar_t text[32]{};
        GetPrivateProfileStringW(L"Display", L"FieldOfView", L"", text, 32, path.c_str());
        check(std::wstring_view(text) == L"93.125", "keyboard save changed display setting");
        GetPrivateProfileStringW(L"Other", L"Preserve", L"", text, 32, path.c_str());
        check(std::wstring_view(text) == L"yes", "keyboard save changed unrelated section");
        const auto saved = bytes(path);
        auto invalid = selected; invalid.keys[size_t(KeyboardAction::Reload)][0] = VK_ESCAPE;
        check(!saveKeyboardBindings(path, invalid) && bytes(path) == saved, "invalid save changed file");
        const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "cannot lock isolated fixture");
        const bool lockWrite = saveKeyboardBindings(path, defaults);
        CloseHandle(locked);
        check(!lockWrite && bytes(path) == saved && loadKeyboardBindings(path) == selected,
              "failed final replacement partially changed controls");
        check(saveKeyboardBindings(path, defaults), "cannot restore defaults");
        for (const auto malformed : {L"-1", L"256", L"1oops", L"42949672960", L"27"}) {
            WritePrivateProfileStringW(L"Keyboard", L"MoveForwardPrimary", malformed, path.c_str());
            check(loadKeyboardBindings(path) == defaults, "malformed saved key did not recover");
        }
        WritePrivateProfileStringW(L"Keyboard", L"MoveForwardPrimary", L"69", path.c_str());
        check(loadKeyboardBindings(path) == defaults, "duplicate saved key did not recover complete defaults");
        WritePrivateProfileStringW(L"Keyboard", L"MoveForwardPrimary", L"0", path.c_str());
        auto unbound = defaults; unbound.keys[size_t(KeyboardAction::MoveForward)][0] = 0;
        check(loadKeyboardBindings(path) == unbound, "explicit unbound key treated as missing");
        check(!keyboardKeyName(VK_UP).empty() && keyboardKeyPrompt('P') == "P" && keyboardKeyPrompt(VK_CONTROL) == "Ct",
              "key labels unavailable");
        auto legacy = defaults;
        legacy.keys[size_t(KeyboardAction::FireRight)][1] = 0;
        legacy.keys[size_t(KeyboardAction::FireLeft)][1] = 0;
        legacy.keys[size_t(KeyboardAction::Zoom)][1] = 0;
        check(saveKeyboardBindings(path, legacy) && loadKeyboardBindings(path) == legacy,
              "versioned explicit mouse unbinding was restored");
        check(WritePrivateProfileStringW(L"Keyboard", L"BindingsVersion", nullptr, path.c_str()) &&
              loadKeyboardBindings(path) == defaults, "legacy mouse controls were not restored");
        auto legacyCustom = legacy;
        check(assignKeyboardKey(legacyCustom, KeyboardAction::FireRight, 0, 'P') &&
              assignKeyboardKey(legacyCustom, KeyboardAction::FireRight, 1, 'O') &&
              assignKeyboardKey(legacyCustom, KeyboardAction::FireLeft, 0, 0) &&
              assignKeyboardKey(legacyCustom, KeyboardAction::FireLeft, 1, 'Z') &&
              saveKeyboardBindings(path, legacyCustom) &&
              WritePrivateProfileStringW(L"Keyboard", L"BindingsVersion", nullptr, path.c_str()),
              "cannot prepare legacy custom controls");
        auto migratedCustom = legacyCustom;
        migratedCustom.keys[size_t(KeyboardAction::FireLeft)][0] = VK_RBUTTON;
        migratedCustom.keys[size_t(KeyboardAction::Zoom)][1] = VK_MBUTTON;
        check(loadKeyboardBindings(path) == migratedCustom,
              "legacy migration displaced custom keys or missed the primary vacancy");
        auto legacyReassigned = legacy;
        check(assignKeyboardKey(legacyReassigned, KeyboardAction::Use, 1, VK_LBUTTON) &&
              saveKeyboardBindings(path, legacyReassigned) &&
              WritePrivateProfileStringW(L"Keyboard", L"BindingsVersion", nullptr, path.c_str()),
              "cannot prepare explicitly reassigned legacy mouse fixture");
        auto migratedReassigned = legacyReassigned;
        migratedReassigned.keys[size_t(KeyboardAction::FireLeft)][1] = VK_RBUTTON;
        migratedReassigned.keys[size_t(KeyboardAction::Zoom)][1] = VK_MBUTTON;
        check(loadKeyboardBindings(path) == migratedReassigned,
              "legacy migration duplicated an explicitly reassigned mouse button");
        std::puts("Keyboard bindings: keyboard/mouse controls, conflict swaps, atomic persistence and legacy migration passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Keyboard bindings: %s\n", error.what()); return 1;
    }
}
