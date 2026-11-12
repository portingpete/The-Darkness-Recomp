#include "runtime/native/movement_settings.h"
#include <windows.h>
#include <fstream>
#include <stdexcept>
#include <cstdio>

using namespace DarkRecomp::Native;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"DarkRecomp-movement-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    struct Cleanup {
        std::filesystem::path path;
        bool owned = false;
        ~Cleanup() { if (owned) { SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(path.c_str()); } }
    } cleanup{path};
    try {
        check(!std::filesystem::exists(path), "isolated fixture already exists");
        cleanup.owned = true;
        check(loadMovementStyle(path) == MovementStyle::Original, "missing setting defaults to original");
        check(!std::filesystem::exists(path), "read-only load created a settings file");
        check(WritePrivateProfileStringW(L"Game", L"Language", L"it", path.c_str()) &&
              WritePrivateProfileStringW(L"Display", L"FieldOfView", L"95", path.c_str()) &&
              WritePrivateProfileStringW(L"Keyboard", L"Forward", L"87", path.c_str()), "seed settings fixture");
        for (auto style : {MovementStyle::Original, MovementStyle::Alternate}) {
            check(setMovementStyle(style) && movementStyle() == style, "apply preset");
            check(saveMovementStyle(path, style) && loadMovementStyle(path) == style, "persist preset");
            wchar_t text[32]{};
            GetPrivateProfileStringW(L"Game", L"Language", L"", text, 32, path.c_str());
            check(std::wstring_view(text) == L"it", "movement save changed language");
            check(GetPrivateProfileIntW(L"Display", L"FieldOfView", 0, path.c_str()) == 95 &&
                  GetPrivateProfileIntW(L"Keyboard", L"Forward", 0, path.c_str()) == 87, "movement save changed other settings");
        }
        for (const auto* invalid : {L"", L"1", L"HL2garbage", L"Doom Eternal", L"-1", L"original", L"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}) {
            check(WritePrivateProfileStringW(L"Game", L"MovementStyle", invalid, path.c_str()) != 0, "seed invalid preset");
            const auto before = bytes(path);
            check(loadMovementStyle(path) == MovementStyle::Original && bytes(path) == before, "invalid preset handling mutates file");
        }
        for (const auto* legacy : {L"doom", L"DOOM", L"ALTERNATE"}) {
            check(WritePrivateProfileStringW(L"Game", L"MovementStyle", legacy, path.c_str()) != 0, "seed compatible name");
            const auto original = bytes(path);
            check(loadMovementStyle(path) == MovementStyle::Alternate && bytes(path) == original,
                  "renamed setting must load without rewriting user preferences");
        }
        check(WritePrivateProfileStringW(L"Game", L"MovementStyle", L"hl2", path.c_str()) &&
              loadMovementStyle(path) == MovementStyle::Original, "removed preset defaults to Original");
        check(saveMovementStyle(path, MovementStyle::Alternate), "save canonical alternate setting");
        wchar_t canonical[32]{};
        GetPrivateProfileStringW(L"Game", L"MovementStyle", L"", canonical, 32, path.c_str());
        check(std::wstring_view(canonical) == L"alternate", "save must use renamed setting");
        const auto before = bytes(path);
        check(!setMovementStyle(MovementStyle(2)) && !saveMovementStyle(path, MovementStyle(2)) && bytes(path) == before,
              "invalid enum must not change settings");
        check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "set read-only fixture");
        check(!saveMovementStyle(path, MovementStyle::Alternate) && bytes(path) == before, "failed save changed existing file");
        requestMovementStyleSave();
        check(takeMovementStyleSaveRequest() && !takeMovementStyleSaveRequest(), "save request consumed once");
        reportMovementStyleSave(false); check(movementStyleSaveFailed(), "failed save status");
        reportMovementStyleSave(true); check(!movementStyleSaveFailed(), "save status recovery");
        puts("Movement settings: defaults, presets, invalid input, isolated persistence and failed saves passed.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
