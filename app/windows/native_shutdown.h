#pragma once
#include <windows.h>

namespace DarkRecomp::Native {
// Stop draining the queue as soon as quit is read. A later posted message
// must not overwrite WM_QUIT before the display loop checks it.
inline bool takeNativeMessage(MSG& message) noexcept {
    return PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) && message.message != WM_QUIT;
}

// Original game workers still run at launcher shutdown. ExitProcess kills
// them before DLL_PROCESS_DETACH, which can deadlock if a worker held a lock
// needed by a DLL. Call only after explicit cleanup and log persistence.
[[noreturn]] inline void terminateNativeProcess(UINT status) noexcept {
    TerminateProcess(GetCurrentProcess(), status);
    // Self-termination never returns on success. Keep the function noreturn
    // even if the operating system rejects the process termination request.
    ExitProcess(status);
}
} // namespace DarkRecomp::Native
