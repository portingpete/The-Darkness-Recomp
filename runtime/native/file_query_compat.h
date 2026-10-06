#pragma once
#include "stall_profiler.h"
#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <stdexcept>

namespace DarkRecomp::Native {
struct DirectoryQueryCompletion {
    uint32_t status;
    bool completed;
};

// Wine rejects event-bearing directory queries before consuming any entry.
// Keep the ordinary Windows event path and retry only that unsupported status.
template<class Submit>
DirectoryQueryCompletion queryDirectoryWithCompletion(Submit submit, HANDLE file, HANDLE event,
                                                       IO_STATUS_BLOCK& io) {
    constexpr uint32_t pending = 0x103, notImplemented = 0xc0000002;
    uint32_t status = submit(event);
    const bool eventless = status == notImplemented;
    if (eventless) {
        io = {};
        status = submit(nullptr);
    }
    if (status == pending) {
        StallProfiler::Scope profile(StallProfiler::Section::Wait, "NtQueryDirectoryFile.completion",
            0, 0, reinterpret_cast<uintptr_t>(eventless ? file : event),
            eventless ? "host-file-handle" : "host-file-completion-event");
        const DWORD wait = WaitForSingleObject(eventless ? file : event, INFINITE);
        status = uint32_t(io.Status);
        if (wait != WAIT_OBJECT_0 || status == pending)
            throw std::runtime_error("Native directory completion never signaled");
    }
    // A terminal eventless query can complete with a truncated record or an
    // exhausted/empty search. Submission errors do not fabricate notifications.
    const bool completed = eventless ?
        (int32_t(status) >= 0 || status == 0x80000005 || status == 0x80000006 || status == 0xc000000f) :
        WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    return {status, completed};
}
}
