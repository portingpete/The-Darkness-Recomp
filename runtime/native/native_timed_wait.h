#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace DarkRecomp::Native {

// Preserve the guest's signed 100 ns deadline. Millisecond Win32 timeouts use
// the system timer tick (about 15.6 ms on some hosts), which starves the game's
// 5 ms streaming-audio refill loop even when the output device stays fed.
inline DWORD nativeTimedWait(HANDLE object, BOOL alertable, const int64_t* ticks) {
    if (!ticks) return WaitForSingleObjectEx(object, INFINITE, alertable);
    const DWORD immediate = WaitForSingleObjectEx(object, 0, alertable);
    if (immediate != WAIT_TIMEOUT || !*ticks) return immediate;

    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        bool inUse = false;
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    static thread_local Timer timer;
    struct Lease {
        Timer& timer;
        bool cached;
        HANDLE handle;
        explicit Lease(Timer& value) : timer(value), cached(!value.inUse),
            handle(cached ? value.handle : CreateWaitableTimerExW(nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {
            if (cached) timer.inUse = true;
        }
        ~Lease() {
            if (handle) CancelWaitableTimer(handle);
            if (cached) timer.inUse = false;
            else if (handle) CloseHandle(handle);
        }
    } lease(timer); // An APC may enter another timed wait on this thread.
    LARGE_INTEGER due;
    due.QuadPart = *ticks;
    if (lease.handle && SetWaitableTimer(lease.handle, &due, 0, nullptr, nullptr, FALSE)) {
        const HANDLE handles[] = {object, lease.handle};
        const DWORD result = WaitForMultipleObjectsEx(2, handles, FALSE, INFINITE, alertable);
        return result == WAIT_OBJECT_0 + 1 ? WAIT_TIMEOUT : result;
    }
    // Older hosts may lack high-resolution timers; retain the ordinary wait.
    uint64_t remaining;
    if (*ticks < 0) remaining = uint64_t(-(*ticks + 1)) + 1;
    else {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        const uint64_t current = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        remaining = uint64_t(*ticks) > current ? uint64_t(*ticks) - current : 0;
    }
    const uint64_t millis = remaining / 10000 + (remaining % 10000 != 0);
    return WaitForSingleObjectEx(object, DWORD((std::min)(millis, uint64_t(INFINITE - 1))), alertable);
}

} // namespace DarkRecomp::Native
