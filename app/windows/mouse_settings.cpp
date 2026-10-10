#include "mouse_settings.h"
#include "runtime/native/mouse_sensitivity.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>

namespace DarkRecomp {
using namespace Native;
namespace {
constexpr uint64_t kLegacyMaximumMouseSensitivity = 100;
bool parseDecimalSensitivity(std::wstring_view text, uint64_t maximum, float& value) noexcept {
    if (text.empty() || text.size() > 16) return false;
    uint64_t significand = 0, divisor = 1;
    bool dot = false, digits = false, fraction = false;
    for (const wchar_t c : text) {
        if (c == L'.' && !dot) { dot = true; continue; }
        if (c < L'0' || c > L'9') return false;
        significand = significand * 10 + uint64_t(c - L'0');
        digits = true;
        if (dot) { divisor *= 10; fraction = true; }
    }
    if (!digits || (dot && !fraction)) return false;
    // Compare the decimal bounds before rounding to float. Otherwise values
    // immediately outside the range could round back into an accepted value.
    static_assert(kMinimumMouseSensitivity == 1.f / 10);
    if (significand * 10 < divisor || significand > maximum * divisor)
        return false;
    value = float(double(significand) / double(divisor));
    return true;
}
}

bool parseMouseSensitivity(std::wstring_view text, float& value) noexcept {
    static_assert(kMaximumMouseSensitivity == 10.f);
    float parsed = kDefaultMouseSensitivity;
    if (!parseDecimalSensitivity(text, uint64_t(kMaximumMouseSensitivity), parsed) ||
        !isValidMouseSensitivity(parsed)) return false;
    value = parsed;
    return true;
}

float loadMouseSensitivity(const std::filesystem::path& path) noexcept {
    wchar_t text[32]{};
    const auto length = GetPrivateProfileStringW(L"Mouse", L"Sensitivity", L"", text, 32, path.c_str());
    float value = kDefaultMouseSensitivity;
    if (length >= 31 || !parseDecimalSensitivity(std::wstring_view(text, length),
                                               kLegacyMaximumMouseSensitivity, value))
        return kDefaultMouseSensitivity;
    // Earlier versions accepted values up to 100x. Recover that selection at
    // the current cap while keeping startup and read-only loads nonmutating.
    return std::min(value, kMaximumMouseSensitivity);
}

bool saveMouseSensitivity(const std::filesystem::path& path, float value) noexcept {
    if (!isValidMouseSensitivity(value)) return false;
    // Preserve every valid float, including fractional per-run values that
    // become a menu selection, without using locale-dependent punctuation.
    char decimal[32]{};
    const auto converted = std::to_chars(decimal, decimal + sizeof(decimal), value,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (converted.ec != std::errc{}) return false;
    wchar_t text[32]{};
    for (size_t i = 0; i < size_t(converted.ptr - decimal); ++i) text[i] = wchar_t(decimal[i]);
    try {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_READONLY) return false;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        // Change only the private copy. A failed write or final replacement
        // leaves the original mouse, keyboard and display settings intact.
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempFileNameW(path.parent_path().c_str(), L"dms", 0, temporary)) return false;
        struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{temporary};
        if (attributes != INVALID_FILE_ATTRIBUTES && !CopyFileW(path.c_str(), temporary, FALSE)) return false;
        if (!WritePrivateProfileStringW(L"Mouse", L"Sensitivity", text, temporary)) return false;
        // The profile API returns zero when it flushes its cache successfully.
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary);
        return MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) { return false; }
}
}
