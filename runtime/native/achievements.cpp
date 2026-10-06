#include "achievements.h"
#include "stall_profiler.h"
#include "achievements_catalog.h"
#include "runtime.h"
#include "storage.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string_view>

namespace DarkRecomp::Native::Achievements {
namespace {
std::mutex mutex;
Snapshot state;
std::filesystem::path loadedRoot;
bool progressLoaded = false;
uint32_t progressError = 0;
std::atomic<bool> showRequested{false}, open{false};
std::atomic<uint32_t> sequence{0};
constexpr std::string_view signature = "DarkRecomp achievements v1\n";

// Each award reloads under a shared mutex before merging. Atomic replacement
// alone would lose an unlock written by another running game instance.
struct ProgressLock {
    HANDLE handle = nullptr;
    bool owned = false;
    explicit ProgressLock(const std::filesystem::path& root) {
        std::error_code ec;
        auto path = std::filesystem::weakly_canonical(root, ec).wstring();
        if (ec || path.empty()) return;
        CharLowerBuffW(path.data(), DWORD(path.size()));
        uint64_t hash = 14695981039346656037ull;
        for (const auto c : path) { hash ^= uint16_t(c); hash *= 1099511628211ull; }
        const auto name = L"Local\\DarkRecompAchievements-" + std::to_wstring(hash);
        handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle) return;
        StallProfiler::Scope wait(StallProfiler::Section::Wait, "achievement-progress-mutex",
            currentContext ? currentContext->lastFunction : 0,
            currentContext ? uint32_t(currentContext->lr) : 0,
            reinterpret_cast<uintptr_t>(handle), "host-achievement-progress-mutex");
        const auto result = WaitForSingleObject(handle, 2000);
        owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~ProgressLock() {
        if (owned) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
};

bool plainFile(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    return !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}

void loadProgressLocked() {
    const auto root = Storage::SaveRoot();
    if (progressLoaded && root == loadedRoot) return;
    progressLoaded = true;
    loadedRoot = root;
    progressError = 0;
    for (auto& entry : state.entries) entry.unlockedAt = 0;
    if (state.entries.empty()) return;
    state.error.clear();
    const auto fail = [] {
        progressError = ERROR_READ_FAULT;
        state.error = "Achievement progress could not be read. Existing progress was preserved.";
    };
    if (root.empty()) { fail(); return; }
    const auto path = root / "achievements.dat";
    if (!plainFile(path)) { fail(); return; }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return;
    if (ec || size < signature.size() || size > 16384) { fail(); return; }
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!stream.read(bytes.data(), bytes.size()) || !bytes.starts_with(signature)) { fail(); return; }
    auto pending = state.entries;
    std::string_view text(bytes);
    text.remove_prefix(signature.size());
    while (!text.empty()) {
        const auto end = text.find('\n');
        if (end == text.npos) { fail(); return; }
        const auto line = text.substr(0, end);
        const auto space = line.find(' ');
        uint32_t id = 0;
        uint64_t time = 0;
        if (space == line.npos) { fail(); return; }
        const auto a = std::from_chars(line.data(), line.data() + space, id);
        const auto b = std::from_chars(line.data() + space + 1, line.data() + line.size(), time);
        const auto entry = std::find_if(pending.begin(), pending.end(), [id](const Entry& e) { return e.id == id; });
        if (a.ec != std::errc{} || a.ptr != line.data() + space || b.ec != std::errc{} ||
            b.ptr != line.data() + line.size() || !time || entry == pending.end() || entry->unlockedAt) {
            fail(); return;
        }
        entry->unlockedAt = time;
        text.remove_prefix(end + 1);
    }
    state.entries = std::move(pending);
}

uint32_t saveProgressLocked(const std::vector<Entry>& entries) {
    std::error_code ec;
    if (!Storage::EnsureSaveRoot(ec)) return ERROR_WRITE_FAULT;
    const auto target = loadedRoot / "achievements.dat";
    if (!plainFile(target)) return ERROR_ACCESS_DENIED;
    std::string bytes(signature);
    for (const auto& entry : entries)
        if (entry.unlockedAt) bytes += std::to_string(entry.id) + " " + std::to_string(entry.unlockedAt) + "\n";
    const auto temporary = loadedRoot / (".achievements." + std::to_string(GetCurrentProcessId()) + "." +
                                          std::to_string(sequence.fetch_add(1)) + ".tmp");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0;
    const bool saved = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
                       written == bytes.size() && FlushFileBuffers(file);
    uint32_t result = saved ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    if (!CloseHandle(file) && !result) result = ERROR_WRITE_FAULT;
    if (!result && !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        result = GetLastError();
    if (result) DeleteFileW(temporary.c_str());
    return result;
}
}

void initialize(std::span<const uint8_t> xdbf, uint32_t language, bool russianEnglishSlot) {
    auto entries = parseAchievementCatalog(xdbf, language, russianEnglishSlot);
    std::lock_guard lock(mutex);
    state = {std::move(entries), {}};
    if (state.entries.empty()) state.error = "Achievement information is unavailable in this game dump.";
    progressLoaded = false;
    progressError = 0;
    showRequested = false;
    open = false;
}

void reset() {
    std::lock_guard lock(mutex);
    // Retain the verified catalog; reset session state and reload persisted progress.
    for (auto& entry : state.entries) entry.unlockedAt = 0;
    progressLoaded = false;
    progressError = 0;
    showRequested = false;
    open = false;
}

Snapshot snapshot() {
    std::lock_guard lock(mutex);
    loadProgressLocked();
    return state;
}

uint32_t award(std::span<const uint32_t> ids) {
    StallProfiler::Scope profile(StallProfiler::Section::FileIO, "Achievements::award",
        currentContext ? currentContext->lastFunction : 0, currentContext ? uint32_t(currentContext->lr) : 0);
    std::lock_guard lock(mutex);
    ProgressLock fileLock(Storage::SaveRoot());
    if (!fileLock.owned) {
        state.error = "Achievement progress is busy. Please try again.";
        return ERROR_BUSY;
    }
    progressLoaded = false;
    loadProgressLocked();
    if (progressError) return progressError;
    if (state.entries.empty()) return ERROR_NOT_READY;
    auto pending = state.entries;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t time = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    bool changed = false;
    for (const auto id : ids) {
        const auto entry = std::find_if(pending.begin(), pending.end(), [id](const Entry& e) { return e.id == id; });
        if (entry == pending.end()) return ERROR_INVALID_PARAMETER;
        if (!entry->unlockedAt) { entry->unlockedAt = time; changed = true; }
    }
    if (!changed) return ERROR_SUCCESS;
    const auto result = saveProgressLocked(pending);
    if (result) {
        state.error = "Achievement progress could not be saved. Check the saves folder and try again.";
        return result;
    }
    for (size_t i = 0; i < pending.size(); ++i)
        if (pending[i].unlockedAt && !state.entries[i].unlockedAt)
            std::fprintf(stderr, "[Achievements] Unlocked id=%u points=%u: %s\n",
                pending[i].id, pending[i].gamerscore, pending[i].name.c_str());
    state.entries = std::move(pending);
    state.error.clear();
    return ERROR_SUCCESS;
}

uint32_t requestShow(uint32_t user) {
    if (user != 0) return ERROR_NO_SUCH_USER;
    showRequested.store(true);
    return ERROR_SUCCESS;
}
bool consumeShowRequest() { return showRequested.exchange(false); }
bool viewerOpen() { return open.load(); }
void setViewerOpen(bool value) {
    if (open.exchange(value) != value) publishSystemUiNotification(value);
}
}
