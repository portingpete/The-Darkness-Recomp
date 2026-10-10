#pragma once
#include <filesystem>
#include <string_view>

namespace DarkRecomp {
// Strict 0.1..10 decimal sensitivity values, independent of the process locale.
bool parseMouseSensitivity(std::wstring_view text, float& value) noexcept;
// Legacy valid values above 10 are capped without rewriting the file.
float loadMouseSensitivity(const std::filesystem::path& path) noexcept;
bool saveMouseSensitivity(const std::filesystem::path& path, float value) noexcept;
}
