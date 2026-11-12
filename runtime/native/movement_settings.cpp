#include "movement_settings.h"
#include <windows.h>
#include <atomic>

namespace DarkRecomp::Native {
namespace {
std::atomic<MovementStyle> selected{MovementStyle::Original};
std::atomic<uint64_t> revision{0};
std::atomic<bool> pending{false}, failed{false};
}
MovementStyle movementStyle() noexcept { return selected.load(std::memory_order_acquire); }
uint64_t movementStyleRevision() noexcept { return revision.load(std::memory_order_acquire); }
bool setMovementStyle(MovementStyle style) noexcept {
    if (!validMovementStyle(style)) return false;
    if (selected.exchange(style, std::memory_order_acq_rel) != style)
        revision.fetch_add(1, std::memory_order_release);
    return true;
}
MovementStyle loadMovementStyle(const std::filesystem::path& path) noexcept {
    wchar_t text[32]{};
    const auto size = GetPrivateProfileStringW(L"Game", L"MovementStyle", L"original", text, 32, path.c_str());
    // Preserve existing selections of the renamed option. Removed HL2 falls back to Original.
    if ((size == 9 && _wcsicmp(text, L"alternate") == 0) ||
        (size == 4 && _wcsicmp(text, L"doom") == 0)) return MovementStyle::Alternate;
    return MovementStyle::Original;
}
bool saveMovementStyle(const std::filesystem::path& path, MovementStyle style) noexcept {
    if (!validMovementStyle(style)) return false;
    try {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_READONLY) return false;
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempFileNameW(path.parent_path().c_str(), L"dmv", 0, temporary)) return false;
        struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{temporary};
        if (attributes != INVALID_FILE_ATTRIBUTES && !CopyFileW(path.c_str(), temporary, FALSE)) return false;
        const auto* value = style == MovementStyle::Alternate ? L"alternate" : L"original";
        if (!WritePrivateProfileStringW(L"Game", L"MovementStyle", value, temporary)) return false;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary);
        return MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    } catch (...) { return false; }
}
void requestMovementStyleSave() noexcept { pending.store(true); }
bool takeMovementStyleSaveRequest() noexcept { return pending.exchange(false); }
void reportMovementStyleSave(bool saved) noexcept { failed.store(!saved); }
bool movementStyleSaveFailed() noexcept { return failed.load(); }
}
