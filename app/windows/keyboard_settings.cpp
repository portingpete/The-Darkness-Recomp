#include "keyboard_settings.h"
#include <cstdio>
#include <optional>

namespace DarkRecomp {
using namespace Native;
namespace {
std::wstring settingName(size_t action, size_t slot) {
    return std::wstring(keyboardActions()[action].setting) + (slot ? L"Secondary" : L"Primary");
}
std::optional<unsigned> readKey(const std::filesystem::path& path, const wchar_t* key) {
    wchar_t text[32]{};
    const auto length = GetPrivateProfileStringW(L"Keyboard", key, L"", text, 32, path.c_str());
    if (!length || length >= 31) return {};
    unsigned value = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return {};
        value = value * 10 + unsigned(text[i] - L'0');
        if (value > 255) return {};
    }
    return assignableKeyboardKey(value) ? std::optional<unsigned>(value) : std::nullopt;
}
}
KeyboardBindings loadKeyboardBindings(const std::filesystem::path& path) noexcept {
    const auto defaults = defaultKeyboardBindings();
    try {
        auto loaded = defaults;
        for (size_t action = 0; action < kKeyboardActionCount; ++action)
            for (size_t slot = 0; slot < kKeyboardBindingSlots; ++slot) {
                const auto value = readKey(path, settingName(action, slot).c_str());
                if (value) loaded.keys[action][slot] = uint16_t(*value);
            }
        // Corrupt/partial files may introduce collisions with a default key.
        // Keep the complete default selection rather than drop another action.
        if (!validKeyboardBindings(loaded)) {
            std::fputs("[Keyboard] Conflicting saved keys; using defaults.\n", stderr);
            return defaults;
        }
        return loaded;
    } catch (...) { return defaults; }
}
bool saveKeyboardBindings(const std::filesystem::path& path, const KeyboardBindings& bindings) noexcept {
    if (!validKeyboardBindings(bindings)) return false;
    try {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempFileNameW(path.parent_path().c_str(), L"dks", 0, temporary)) return false;
        struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{temporary};
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(path.c_str(), temporary, FALSE)) return false;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        for (size_t action = 0; action < kKeyboardActionCount; ++action)
            for (size_t slot = 0; slot < kKeyboardBindingSlots; ++slot) {
                const auto text = std::to_wstring(bindings.keys[action][slot]);
                if (!WritePrivateProfileStringW(L"Keyboard", settingName(action, slot).c_str(),
                                                text.c_str(), temporary)) return false;
            }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary);
        return MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) { return false; }
}
}
