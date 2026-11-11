#pragma once
// Opt-in --test-input line parser. This application's explicit own-process
// test interface only: it maps bounded numeric key codes into the same native
// input adapter exercised by InputContract, or F5/F6's own window handlers.
// No OS-wide injection, SendInput or guest-data patch. Ordinary gameplay keys
// preserve the existing focus/controller rules; F5 opens the developer panel,
// while F6 requests the live render-resolution toggle.
// Strict grammar: "<key>" or "<key> <holdMs>", nothing else. Bare numeric
// lines keep historical holds (camera I/J/K/L 2000ms, other menu keys 250ms).
// Anything malformed returns false with outputs untouched, so the caller
// stays blocked on the same unconsumed line without side effects.
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

inline constexpr size_t kTestInputMaxLines = 64;
inline constexpr size_t kTestInputMaxFileBytes = 64 * 4096;

// Newline terminates a command: an append/rewrite must not turn a partially
// written key or duration into an input event. Preserve CRLF and line indexes.
inline std::vector<std::string> completeTestInputLines(std::string_view bytes) {
    std::vector<std::string> lines;
    size_t begin = 0;
    while (lines.size() < kTestInputMaxLines) {
        const auto end = bytes.find('\n', begin);
        if (end == std::string_view::npos) break;
        auto line = bytes.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.emplace_back(line);
        begin = end + 1;
    }
    return lines;
}

// Diagnostic script files can be edited while a replay runs. Keep all file
// opens, reads and parsing on one owned worker; the display only borrows an
// immutable snapshot, skipping this loop if its short publication lock is busy.
// Two equal reads avoid publishing a transient truncate/rewrite. At 250 ms
// polling, an ordinary completed append is visible within about 500 ms.
class NativeTestInputFileReader {
public:
    using Snapshot = std::shared_ptr<const std::vector<std::string>>;
    using Read = std::function<std::optional<std::string>(const std::filesystem::path&, HANDLE)>;

    static std::optional<std::string> readFile(const std::filesystem::path& path, HANDLE stop) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return {};
        struct Handle {
            HANDLE value;
            ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } file{CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return {};
        BY_HANDLE_FILE_INFORMATION before{}, after{};
        if (!GetFileInformationByHandle(file.value, &before) || before.nFileSizeHigh ||
            before.nFileSizeLow > kTestInputMaxFileBytes) return {};
        std::string bytes(before.nFileSizeLow, '\0');
        if (!bytes.empty()) {
            Handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            if (!completed.value) return {};
            OVERLAPPED operation{}; operation.hEvent = completed.value;
            DWORD count = 0;
            if (!ReadFile(file.value, bytes.data(), DWORD(bytes.size()), nullptr, &operation)) {
                if (GetLastError() != ERROR_IO_PENDING) return {};
                const HANDLE events[]{stop, completed.value};
                const auto result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (result != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(file.value, &operation);
                    // Storage owns the OVERLAPPED/buffer until cancellation
                    // completes. Never detach a worker with live stack I/O.
                    GetOverlappedResult(file.value, &operation, &count, TRUE);
                    return {};
                }
            }
            // Both immediate and pending overlapped completions report their
            // transferred bytes through the OVERLAPPED result, not ReadFile.
            if (!GetOverlappedResult(file.value, &operation, &count, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) {
                    CancelIoEx(file.value, &operation);
                    GetOverlappedResult(file.value, &operation, &count, TRUE);
                }
                return {};
            }
            if (count != bytes.size()) return {};
        }
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
            !GetFileInformationByHandle(file.value, &after) ||
            before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
            CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0) return {};
        return bytes;
    }

    // A separate reader can request a normal close independently of script
    // parsing, its command limit, or a blocked script-file read. Only an exact
    // complete line qualifies; the existing two-equal-read publication rule
    // prevents a partial/truncated write from requesting shutdown.
    static std::optional<std::string> readQuitFile(const std::filesystem::path& path, HANDLE stop) {
        const auto bytes = readFile(path, stop);
        if (bytes && (*bytes == "quit\n" || *bytes == "quit\r\n")) return "quit\n";
        return {};
    }

    explicit NativeTestInputFileReader(const std::filesystem::path& path, Read read = readFile,
                                      DWORD pollMs = 250) {
        if (path.empty()) return;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) throw std::runtime_error("Cannot create diagnostic input stop event");
        try {
            worker_ = std::thread([this, path, read = std::move(read), pollMs] {
                std::optional<std::string> candidate, published;
                while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
                    try {
                        auto bytes = read(path, stop_);
                        if (bytes && bytes->size() <= kTestInputMaxFileBytes) {
                            if (candidate && *candidate == *bytes && (!published || *published != *bytes)) {
                                auto next = std::make_shared<const std::vector<std::string>>(completeTestInputLines(*bytes));
                                Snapshot previous;
                                {
                                    std::lock_guard lock(mutex_);
                                    previous = std::exchange(snapshot_, std::move(next));
                                }
                                // Retire the old snapshot outside the publication lock.
                                published = *bytes;
                            }
                            candidate = std::move(bytes);
                        } else candidate.reset();
                    } catch (...) { candidate.reset(); }
                    if (WaitForSingleObject(stop_, pollMs ? pollMs : 1) != WAIT_TIMEOUT) break;
                }
            });
        } catch (...) { CloseHandle(stop_); stop_ = nullptr; throw; }
    }
    ~NativeTestInputFileReader() { stop(); }
    NativeTestInputFileReader(const NativeTestInputFileReader&) = delete;
    NativeTestInputFileReader& operator=(const NativeTestInputFileReader&) = delete;
    Snapshot latest() {
        std::unique_lock lock(mutex_, std::try_to_lock);
        return lock.owns_lock() ? snapshot_ : Snapshot{};
    }
    void stop() {
        if (stop_) SetEvent(stop_);
        if (worker_.joinable()) worker_.join();
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
    }
private:
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::mutex mutex_;
    Snapshot snapshot_;
};

// A recording is a complete script written in advance: it plays unattended
// from launch to exit. "wait <ms 1..600000>" holds the commands after it for
// that long; "wait world" until the game renders its world, "wait menu"
// until a mission can be loaded and "wait player" until a player is active
// (the last two are published while the developer panel is open).
inline constexpr ULONGLONG kTestInputMaxWaitMs = 600000;
enum class TestInputWait { Milliseconds, World, Menu, Player };
inline bool parseTestInputWaitLine(const std::string& command, TestInputWait& kind, ULONGLONG& waitMs) {
    if (command.empty() || command.size() > 256) return false;
    std::istringstream fields(command);
    std::string first, second, extra;
    if (!(fields >> first >> second) || (fields >> extra) || first != "wait") return false;
    if (second == "world") { kind = TestInputWait::World; waitMs = 0; return true; }
    if (second == "menu") { kind = TestInputWait::Menu; waitMs = 0; return true; }
    if (second == "player") { kind = TestInputWait::Player; waitMs = 0; return true; }
    if (second.size() > 6) return false;
    for (char c : second) if (c < '0' || c > '9') return false;
    const ULONGLONG parsed = ULONGLONG(std::strtoull(second.c_str(), nullptr, 10));
    if (!parsed || parsed > kTestInputMaxWaitMs) return false;
    kind = TestInputWait::Milliseconds; waitMs = parsed;
    return true;
}

inline constexpr ULONGLONG kTestInputMaxHoldMs = 10000;
inline constexpr ULONGLONG kTestInputMenuHoldMs = 250;
inline constexpr ULONGLONG kTestInputCameraHoldMs = 2000;
inline bool isTestInputKey(unsigned key) {
    switch (key) {
        case VK_RETURN: case VK_SPACE: case VK_ESCAPE: case VK_F5: case VK_F6:
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_TAB: case VK_BACK: case VK_SHIFT: case VK_CONTROL:
        case 'W': case 'A': case 'S': case 'D':
        case 'E': case 'R': case 'F': case 'X': case 'Z': case 'C':
        case 'Q': case 'G':
        case '1': case '2': case '3': case '4':
        case 'I': case 'J': case 'K': case 'L':
            return true;
        default: return false;
    }
}
inline bool parseTestInputKeyLine(const std::string& command, unsigned& key, ULONGLONG& holdMs) {
    if (command.empty() || command.size() > 256) return false;
    std::istringstream fields(command);
    std::string first, second, extra;
    if (!(fields >> first)) return false;
    const bool hasSecond = bool(fields >> second);
    if (fields >> extra) return false;
    if (first.empty() || first.size() > 3) return false;
    for (char c : first) if (c < '0' || c > '9') return false;
    const unsigned parsed = unsigned(std::strtoul(first.c_str(), nullptr, 10));
    if (!parsed || parsed > 255 || !isTestInputKey(parsed)) return false;
    ULONGLONG hold = 0;
    if (hasSecond) {
        if (second.empty() || second.size() > 5) return false;
        for (char c : second) if (c < '0' || c > '9') return false;
        hold = ULONGLONG(std::strtoull(second.c_str(), nullptr, 10));
        if (!hold || hold > kTestInputMaxHoldMs) return false;
    } else {
        const bool cameraKey = parsed == 'I' || parsed == 'J' || parsed == 'K' || parsed == 'L';
        hold = cameraKey ? kTestInputCameraHoldMs : kTestInputMenuHoldMs;
    }
    key = parsed; holdMs = hold;
    return true;
}
