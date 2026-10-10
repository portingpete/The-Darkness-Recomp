#include "app/windows/native_launch_paths.h"
#include <windows.h>
#include <cstdio>
#include <fstream>

namespace {
namespace fs = std::filesystem;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void gameFile(const fs::path& root) {
    fs::create_directories(root / L"Darkness");
    std::ofstream(root / L"Darkness/default.xex") << "fixture";
}
void requireMissing(const fs::path& executable) {
    bool missing = false;
    try { DarkRecomp::findInstalledGameDirectory(executable); }
    catch (const std::runtime_error&) { missing = true; }
    require(missing, "Missing assets selected an unrelated game directory");
}
}

int main() {
    const auto originalDirectory = fs::current_path();
    const auto root = fs::temp_directory_path() /
        (L"DarkRecomp launch paths & \u00e9 " + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    // Create a fresh, owned fixture. Restore cwd before removing this directory.
    if (!fs::create_directory(root)) return 1;
    int result = 0;
    try {
        const auto installation = root / L"installed game";
        const auto binary = installation / L"build_native/Release/DarkRecomp.exe";
        fs::create_directories(binary.parent_path());
        gameFile(installation);
        const auto unrelated = root / L"unrelated working directory";
        gameFile(unrelated);
        fs::current_path(unrelated);
        require(DarkRecomp::findInstalledGameDirectory(binary) == installation / L"Darkness",
                "Release layout was resolved from cwd instead of the EXE");

        gameFile(binary.parent_path());
        require(DarkRecomp::findInstalledGameDirectory(binary) == binary.parent_path() / L"Darkness",
                "Assets beside the EXE did not take priority");
        requireMissing(root / L"missing/Release/DarkRecomp.exe");

        const auto directoryOnly = root / L"directory-only";
        fs::create_directories(directoryOnly / L"Darkness/default.xex");
        requireMissing(directoryOnly / L"DarkRecomp.exe");
        const auto distant = root / L"distant";
        gameFile(distant);
        requireMissing(distant / L"one/two/three/four/DarkRecomp.exe");
        puts("Native executable-relative launch paths passed.");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        result = 1;
    }
    fs::current_path(originalDirectory);
    fs::remove_all(root);
    return result;
}
