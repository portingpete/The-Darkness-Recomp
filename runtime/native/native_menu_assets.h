#pragma once
#include <filesystem>

namespace DarkRecomp::Native {
struct NativeMenuAssets {
    std::filesystem::path menu;
    std::filesystem::path archive;
};

// Prepare a private startup cache using this dump's original assets. Empty
// paths preserve the original menus when the supplied menu differs from the
// source used to generate the bundled PC menu, or preparation fails.
NativeMenuAssets prepareNativeMenuAssets(const std::filesystem::path& gameDirectory,
                                        const std::filesystem::path& bundledAssetsDirectory) noexcept;
}
