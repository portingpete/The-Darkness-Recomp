#include "display_settings.h"
#include "runtime/native/fov_settings.h"
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <algorithm>

namespace DarkRecomp {
using namespace Native;
namespace {
unsigned readUnsigned(const std::filesystem::path& path, const wchar_t* key,
                      unsigned fallback, unsigned minimum, unsigned maximum) noexcept {
    wchar_t text[32]{};
    const DWORD length = GetPrivateProfileStringW(L"Display", key, L"", text, 32, path.c_str());
    if (!length || length >= 31) return fallback;
    unsigned value = 0;
    for (DWORD i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9') return fallback;
        const unsigned digit = unsigned(text[i] - L'0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return fallback;
        value = value * 10 + digit;
    }
    return value >= minimum ? value : fallback;
}
bool writeUnsigned(const std::filesystem::path& path, const wchar_t* key, unsigned value) {
    wchar_t text[32]{}; swprintf_s(text, L"%u", value);
    return WritePrivateProfileStringW(L"Display", key, text, path.c_str()) != FALSE;
}
}

bool parseFieldOfView(std::wstring_view text, float& value) noexcept {
    if (text.empty() || text.size() > 16) return false;
    double number = 0, divisor = 1;
    bool dot = false, digits = false, fraction = false;
    for (const wchar_t c : text) {
        if (c == L'.' && !dot) { dot = true; continue; }
        if (c < L'0' || c > L'9') return false;
        digits = true;
        if (dot) { divisor *= 10; number += (c - L'0') / divisor; fraction = true; }
        else number = number * 10 + c - L'0';
        if (number > kMaximumConfiguredFov) return false;
    }
    if (!digits || (dot && !fraction)) return false;
    const auto result = float(number);
    if (!isValidConfiguredHorizontalFovDegrees(result)) return false;
    value = result; return true;
}

float loadFieldOfView(const std::filesystem::path& path) noexcept {
    wchar_t text[32]{};
    const DWORD length = GetPrivateProfileStringW(L"Display", L"FieldOfView", L"0", text, 32, path.c_str());
    float value = 0;
    if (length >= 31 || !parseFieldOfView(text, value))
        std::fprintf(stderr, "[Display] Invalid saved field of view; using Original.\n");
    return value;
}

bool saveFieldOfView(const std::filesystem::path& path, float value) noexcept {
    if (!isValidConfiguredHorizontalFovDegrees(value)) return false;
    // Integer thousandths avoid locale-dependent decimal separators. The
    // runtime accepts fractional CLI values; opening the menu preserves them.
    const auto thousandths = unsigned(std::lround(value * 1000));
    wchar_t text[32]{};
    swprintf_s(text, L"%u.%03u", thousandths / 1000, thousandths % 1000);
    return WritePrivateProfileStringW(L"Display", L"FieldOfView", text, path.c_str()) != FALSE;
}

GraphicsSettings loadGraphicsSettings(const std::filesystem::path& path) noexcept {
    const GraphicsSettings defaults;
    return {readUnsigned(path, L"FrameRateLimit", defaults.frameRateLimit, 0, 1000),
            readUnsigned(path, L"RenderHeight", defaults.renderHeight, 180, kMaximumRenderHeight),
            readUnsigned(path, L"VerticalSync", defaults.verticalSync, 0, 1) != 0,
            readUnsigned(path, L"Fullscreen", defaults.fullscreen, 0, 1) != 0,
            readUnsigned(path, L"Bloom", defaults.bloom, 0, 1) != 0,
            readUnsigned(path, L"MotionBlur", defaults.motionBlur, 0, 1) != 0,
            readUnsigned(path, L"Antialiasing", defaults.antialiasing, 0, 1) != 0,
            readUnsigned(path, L"GammaPercent", defaults.gammaPercent, 50, 150),
            readUnsigned(path, L"BrightnessPercent", defaults.brightnessPercent, 50, 200)};
}

bool saveDisplaySettings(const std::filesystem::path& path, float fov, const GraphicsSettings& settings) noexcept {
    if (!isValidConfiguredHorizontalFovDegrees(fov) || !validGraphicsSettings(settings)) return false;
    try {
        // Commit the complete selection together. Work on a private adjacent
        // copy so a write failure cannot leave half of the settings saved.
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempFileNameW(path.parent_path().c_str(), L"dgs", 0, temporary)) return false;
        struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{temporary};
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(path.c_str(), temporary, FALSE)) return false;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        const std::filesystem::path staging(temporary);
        if (!saveFieldOfView(staging, fov) ||
            !writeUnsigned(staging, L"FrameRateLimit", settings.frameRateLimit) ||
            !writeUnsigned(staging, L"RenderHeight", settings.renderHeight) ||
            !writeUnsigned(staging, L"VerticalSync", settings.verticalSync) ||
            !writeUnsigned(staging, L"Fullscreen", settings.fullscreen) ||
            !writeUnsigned(staging, L"Bloom", settings.bloom) ||
            !writeUnsigned(staging, L"MotionBlur", settings.motionBlur) ||
            !writeUnsigned(staging, L"Antialiasing", settings.antialiasing) ||
            !writeUnsigned(staging, L"GammaPercent", settings.gammaPercent) ||
            !writeUnsigned(staging, L"BrightnessPercent", settings.brightnessPercent)) return false;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary);
        return MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) { return false; }
}

}
