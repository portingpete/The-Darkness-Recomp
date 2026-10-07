#include <windows.h>

namespace {
HANDLE workerLock = nullptr;
HANDLE workerReady = nullptr;
HANDLE detachEntered = nullptr;
volatile LONG armed = 0;

DWORD WINAPI holdWorkerLock(void*) {
    if (WaitForSingleObject(workerLock, INFINITE) != WAIT_OBJECT_0) return 1;
    InterlockedExchange(&armed, 1);
    SetEvent(workerReady);
    // Model a title/device worker still running when the launcher exits.
    // Its lock deliberately stays owned until process termination.
    Sleep(INFINITE);
    return 0;
}
}

extern "C" __declspec(dllexport) BOOL WINAPI ArmNativeShutdownDetachFixture(
    HANDLE ready, HANDLE detach) {
    if (!ready || !detach || workerReady) return FALSE;
    workerReady = ready;
    detachEntered = detach;
    workerLock = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    if (!workerLock) return FALSE;
    HANDLE worker = CreateThread(nullptr, 0, holdWorkerLock, nullptr, 0, nullptr);
    if (!worker) { CloseHandle(workerLock); workerLock = nullptr; return FALSE; }
    CloseHandle(worker);
    return WaitForSingleObject(workerReady, 5000) == WAIT_OBJECT_0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    if (reason == DLL_PROCESS_DETACH && InterlockedCompareExchange(&armed, 0, 0)) {
        SetEvent(detachEntered);
        // ExitProcess has already killed the worker. A binary semaphore
        // used as a lock has no abandoned-owner recovery, so detach blocks.
        // Native critical sections/SRW locks may terminate immediately
        // rather than block during process exit and cannot prove this case.
        WaitForSingleObject(workerLock, INFINITE);
        ReleaseSemaphore(workerLock, 1, nullptr);
    }
    return TRUE;
}
