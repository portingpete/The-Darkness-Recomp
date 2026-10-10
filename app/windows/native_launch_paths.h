#pragma once
#include <filesystem>
#include <stdexcept>

namespace DarkRecomp {
// Releases keep the EXE in build_native/Release and Darkness at the release
// root. Resolve from the EXE so desktop and Steam launches do not depend on cwd.
inline std::filesystem::path findInstalledGameDirectory(const std::filesystem::path& executable) {
    auto directory = std::filesystem::absolute(executable).parent_path();
    for (unsigned depth = 0; depth < 4; ++depth) {
        const auto game = directory / L"Darkness";
        std::error_code error;
        if (std::filesystem::is_regular_file(game / L"default.xex", error)) return game;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    throw std::runtime_error("Cannot find Darkness/default.xex near DarkRecomp.exe. "
        "Keep the extracted release folder layout, copy your game dump into Darkness, "
        "or specify --game-dir <directory>.");
}
}
