#include "app/windows/native_shutdown.h"
#include <windows.h>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {
constexpr DWORD startupTimeoutMs = 5000;
constexpr DWORD exitTimeoutMs = 2000;
constexpr DWORD baselineObservationMs = 250;
constexpr UINT fixtureFailure = 101;
constexpr UINT cleanupStatus = 102;
int failures = 0;

struct OwnedHandle {
    HANDLE value = nullptr;
    ~OwnedHandle() { if (value) CloseHandle(value); }
};

void check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s (Win32 error %lu)\n", message, GetLastError()); ++failures; }
}

bool parseUnsigned(const wchar_t* text, unsigned long long& value) {
    if (!text || !*text || *text == L'-') return false;
    wchar_t* end = nullptr;
    value = std::wcstoull(text, &end, 10);
    return end && !*end;
}

[[noreturn]] void child(const wchar_t* mode, const wchar_t* fixturePath,
                      UINT status, HANDLE ready, HANDLE detach) {
    HMODULE fixture = LoadLibraryExW(fixturePath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    using ArmFixture = BOOL (WINAPI*)(HANDLE, HANDLE);
    auto arm = fixture ? reinterpret_cast<ArmFixture>(
        GetProcAddress(fixture, "ArmNativeShutdownDetachFixture")) : nullptr;
    if (!arm || !arm(ready, detach))
        DarkRecomp::Native::terminateNativeProcess(fixtureFailure);
    if (!std::wcscmp(mode, L"exit-process")) ExitProcess(status);
    DarkRecomp::Native::terminateNativeProcess(status);
}

void runCase(const std::wstring& executable, const std::wstring& fixturePath,
             UINT status, bool baseline) {
    const int failuresBefore = failures;
    SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
    OwnedHandle ready{CreateEventW(&inherited, TRUE, FALSE, nullptr)};
    OwnedHandle detach{CreateEventW(&inherited, TRUE, FALSE, nullptr)};
    check(ready.value && detach.value, "create child synchronization events");
    if (!ready.value || !detach.value) return;
    std::wstring command = L"\"" + executable + L"\" --child " +
        (baseline ? L"exit-process" : L"terminate-process") + L" \"" + fixturePath +
        L"\" " + std::to_wstring(status) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(ready.value)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(detach.value));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
          "launch shutdown child");
    if (!process.hProcess) return;
    OwnedHandle childProcess{process.hProcess}, childThread{process.hThread};
    const HANDLE startupWait[] = {ready.value, childProcess.value};
    const DWORD initialized = WaitForMultipleObjects(2, startupWait, FALSE, startupTimeoutMs);
    check(initialized == WAIT_OBJECT_0, "worker acquired fixture lock before shutdown");
    if (initialized == WAIT_OBJECT_0) {
        if (baseline) {
            const HANDLE detachWait[] = {detach.value, childProcess.value};
            check(WaitForMultipleObjects(2, detachWait, FALSE, startupTimeoutMs) == WAIT_OBJECT_0,
                  "ExitProcess reached DLL_PROCESS_DETACH");
            check(WaitForSingleObject(childProcess.value, baselineObservationMs) == WAIT_TIMEOUT,
                  "baseline ExitProcess blocks on the terminated worker's lock");
        } else {
            check(WaitForSingleObject(childProcess.value, exitTimeoutMs) == WAIT_OBJECT_0,
                  "native process termination completed within its deadline");
            DWORD result = STILL_ACTIVE;
            check(GetExitCodeProcess(childProcess.value, &result) && result == status,
                  "native process termination preserves the chosen exit status");
            check(WaitForSingleObject(detach.value, 0) == WAIT_TIMEOUT,
                  "native process termination bypasses DLL detach");
        }
    }
    // Cleanup uses only the exact process handle returned by this CreateProcess.
    // No other game, shell, or process-name match can be affected.
    if (WaitForSingleObject(childProcess.value, 0) == WAIT_TIMEOUT) {
        check(TerminateProcess(childProcess.value, cleanupStatus) != FALSE,
              "terminate only the fixture child after observation or failure");
        check(WaitForSingleObject(childProcess.value, exitTimeoutMs) == WAIT_OBJECT_0,
              "fixture child cleanup completed within its deadline");
    }
    if (failures == failuresBefore)
        std::printf("PASS: %s status=%u with a live worker lock%s\n",
                    baseline ? "ExitProcess baseline deadlock observed" : "native termination",
                    status, baseline ? " (exact child cleaned up)" : "");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 7 && !std::wcscmp(argv[1], L"--child")) {
        unsigned long long status = 0, ready = 0, detach = 0;
        const bool knownMode = !std::wcscmp(argv[2], L"terminate-process") ||
                               !std::wcscmp(argv[2], L"exit-process");
        if (!knownMode || !parseUnsigned(argv[4], status) || status > UINT_MAX ||
            !parseUnsigned(argv[5], ready) || !parseUnsigned(argv[6], detach)) return fixtureFailure;
        child(argv[2], argv[3], UINT(status), reinterpret_cast<HANDLE>(uintptr_t(ready)),
              reinterpret_cast<HANDLE>(uintptr_t(detach)));
    }
    if (argc < 2 || argc > 3 || (argc == 3 && std::wcscmp(argv[2], L"--baseline-exit-process"))) {
        std::fputs("Usage: NativeShutdownTests <absolute fixture DLL path> [--baseline-exit-process]\n", stderr);
        return 1;
    }
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    check(length && length < MAX_PATH, "resolve shutdown test executable");
    if (failures) return 1;
    for (UINT status : {0u, 5u, 6u}) runCase(executable, argv[1], status, false);
    if (argc == 3) runCase(executable, argv[1], 0, true);
    return failures ? 1 : 0;
}
