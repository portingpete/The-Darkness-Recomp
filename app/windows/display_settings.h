#pragma once
#include <windows.h>
#include <filesystem>
#include <string_view>
#include "runtime/native/graphics_settings.h"

namespace DarkRecomp {
// Strict decimal settings, independent of the process locale.
bool parseFieldOfView(std::wstring_view text, float& value) noexcept;
float loadFieldOfView(const std::filesystem::path& path) noexcept;
bool saveFieldOfView(const std::filesystem::path& path, float value) noexcept;
Native::GraphicsSettings loadGraphicsSettings(const std::filesystem::path& path) noexcept;
bool saveDisplaySettings(const std::filesystem::path& path, float fov,
                         const Native::GraphicsSettings& settings) noexcept;

}
