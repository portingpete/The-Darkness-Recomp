#include "runtime.h"
#include "renderer/engine/engine_performance.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <list>

using namespace DarkRecomp::Native;
namespace {
std::mutex dispatcherMutex;
std::condition_variable dispatcherChanged;
thread_local DispatcherCancellation* currentCancellation = nullptr;
thread_local DispatcherWaitResumeHook currentResumeHook = nullptr;
thread_local void* currentResumeContext = nullptr;
constexpr uint32_t invalidParameter = 0xc000000d;
constexpr uint32_t timeoutStatus = 0x102;

void observeWaitResume(std::unique_lock<std::mutex>& lock) {
    if (currentResumeHook) {
        lock.unlock();
        currentResumeHook(currentResumeContext);
        lock.lock();
    }
}

bool span(uint8_t* base, uint32_t address, uint32_t bytes, bool writable = false) {
    if (!base || !address || !bytes || uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    auto* cursor = base + address;
    auto* end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!engineProfileVirtualQuery(EnginePhase::queryDispatcher, cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        DWORD protection = info.Protect & 0xff;
        bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (writable ? !canWrite : (!canWrite && protection != PAGE_READONLY &&
                                               protection != PAGE_EXECUTE_READ)) return false;
        cursor = (std::min)(end, static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize);
    }
    return true;
}
uint32_t read32(uint8_t* base, uint32_t address) {
    uint32_t value;
    memcpy(&value, base + address, 4);
    return _byteswap_ulong(value);
}
void write32(uint8_t* base, uint32_t address, uint32_t value) {
    value = _byteswap_ulong(value);
    memcpy(base + address, &value, 4);
}
bool validObject(uint8_t* base, uint32_t address) {
    if ((address & 3) || !span(base, address, 16, true)) return false;
    uint8_t type = base[address]; // Original PPC initializes Type with stb, not stw.
    uint32_t state = read32(base, address + 4);
    if (type == 0 || type == 1) return state <= 1;
    if (type != 5 || !span(base, address, 20, true)) return false;
    int32_t limit = int32_t(read32(base, address + 16));
    return limit > 0 && state <= uint32_t(limit);
}
void consume(uint8_t* base, uint32_t address) {
    if (base[address] == 1) write32(base, address + 4, 0);
    else if (base[address] == 5) write32(base, address + 4, read32(base, address + 4) - 1);
}

struct PendingWait;
std::list<PendingWait*> pendingWaits;
struct PendingWait {
    uint8_t* base;
    const uint32_t* objects;
    uint32_t count, kind;
    DispatcherCancellation* cancellation;
    bool finite;
    int64_t ticks;
    uint64_t deadline;
    uint32_t result = UINT32_MAX;
    bool allWakePending = false;
    std::list<PendingWait*>::iterator registration = pendingWaits.end();
    uint64_t remainingMilliseconds() const {
        if (!finite) return UINT64_MAX;
        if (ticks < 0) {
            const auto now = GetTickCount64();
            return now < deadline ? deadline - now : 0;
        }
        if (ticks > 0) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            const uint64_t current = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            if (uint64_t(ticks) > current) {
                const uint64_t remaining = uint64_t(ticks) - current;
                return remaining / 10000 + (remaining % 10000 != 0);
            }
        }
        return 0;
    }
    bool expired() const { return finite && !remainingMilliseconds(); }
    void enroll() {
        if (registration == pendingWaits.end()) registration = pendingWaits.insert(pendingWaits.end(), this);
    }
    // Created after the wait's lock: normal return, timeout and cancellation
    // all remove the stack-owned record while dispatcherMutex is still held.
    ~PendingWait() { if (registration != pendingWaits.end()) pendingWaits.erase(registration); }
};
void satisfyWaiters(uint8_t* base, uint32_t address, bool newlySignaled) {
    // Single/any and one-object wait-all acquire when signaled, before their
    // host thread runs. Resetting afterward must not erase that completion.
    for (auto* wait : pendingWaits) {
        if (wait->base != base || wait->result != UINT32_MAX ||
            (wait->cancellation && wait->cancellation->requested)) continue;
        // A host thread may not resume when its timeout elapses. Retire the
        // wait before assigning a later signal, leaving it for live waiters.
        if (wait->expired()) {
            if (!wait->allWakePending) wait->result = timeoutStatus;
            continue;
        }
        if (wait->kind == 0 && wait->count > 1) {
            // A newly signaled member wakes wait-all for a recheck without
            // reserving any objects. Preserve that wake even if its thread
            // cannot run until after the deadline; partial readiness counts.
            if (newlySignaled && std::find(wait->objects, wait->objects + wait->count, address) != wait->objects + wait->count)
                wait->allWakePending = true;
            continue;
        }
        for (uint32_t i = 0; i < wait->count; ++i) if (wait->objects[i] == address) {
            consume(base, address);
            wait->result = i;
            if (!read32(base, address + 4)) return;
            break;
        }
    }
}

uint32_t waitObjects(uint8_t* base, const uint32_t* objects, uint32_t count,
                     uint32_t waitType, uint32_t alertable, uint32_t timeoutPointer) {
    if (!count || count > 64 || waitType > 1) return invalidParameter;
    if (alertable) return 0xc00000bb; // Guest alert/APC delivery is not implemented here.
    if (timeoutPointer && !span(base, timeoutPointer, 8)) return invalidParameter;
    int64_t ticks = 0;
    if (timeoutPointer) {
        uint64_t raw;
        memcpy(&raw, base + timeoutPointer, 8);
        ticks = int64_t(_byteswap_uint64(raw));
    }
    // Relative waits use a monotonic deadline; absolute waits recheck wall time.
    uint64_t interval = ticks < 0 ? uint64_t(-(ticks + 1)) + 1 : 0;
    uint64_t relativeMs = interval / 10000 + (interval % 10000 != 0);
    uint64_t deadline = GetTickCount64() + relativeMs;
    std::unique_lock lock(dispatcherMutex);
    for (uint32_t i = 0; i < count; ++i) {
        if (!validObject(base, objects[i])) return invalidParameter;
        for (uint32_t j = 0; j < i; ++j)
            if (objects[i] == objects[j]) return invalidParameter;
    }
    PendingWait wait{base, objects, count, waitType, currentCancellation, timeoutPointer != 0, ticks, deadline};
    for (;;) {
        // A signal acquired before the deadline remains a completed wait.
        if (wait.result != UINT32_MAX) return wait.result;
        if (currentCancellation && currentCancellation->requested) throw DispatcherWaitCancelled{};
        // Initial zero/expired polls can still acquire an already signaled
        // object. Once blocked, expiry wins over newly available objects.
        if (wait.registration != pendingWaits.end() && wait.expired() && !wait.allWakePending) return timeoutStatus;
        uint32_t first = count;
        bool all = true;
        for (uint32_t i = 0; i < count; ++i) {
            if (read32(base, objects[i] + 4)) { if (first == count) first = i; }
            else all = false;
        }
        if (waitType == 1 && first != count) {
            consume(base, objects[first]);
            return first;
        }
        if (waitType == 0 && all) {
            for (uint32_t i = 0; i < count; ++i) consume(base, objects[i]);
            return 0;
        }
        // A reset or competing acquisition can invalidate the pending wake.
        // A new wait attempt must still respect the original deadline.
        wait.allWakePending = false;
        if (!timeoutPointer) {
            wait.enroll();
            dispatcherChanged.wait(lock);
            observeWaitResume(lock);
            continue;
        }
        uint64_t remainingMs = wait.remainingMilliseconds();
        // Bound the wall-clock recheck so system-time changes take effect.
        if (ticks > 0) remainingMs = (std::min)(uint64_t(100), remainingMs);
        if (!remainingMs) return timeoutStatus;
        // Wait-all reacquires all objects together on the waiting thread.
        wait.enroll();
        dispatcherChanged.wait_for(lock, std::chrono::milliseconds(remainingMs));
        observeWaitResume(lock);
    }
}
}

namespace DarkRecomp::Native {
void setDispatcherWaitResumeHook(DispatcherWaitResumeHook hook, void* context) {
    currentResumeHook = hook;
    currentResumeContext = context;
}
uint32_t dispatcherWaiterCount(uint8_t* base, uint32_t address) {
    std::lock_guard lock(dispatcherMutex);
    uint32_t count = 0;
    for (const auto* wait : pendingWaits) {
        if (wait->base == base && wait->result == UINT32_MAX && (!wait->expired() || wait->allWakePending) &&
            !(wait->cancellation && wait->cancellation->requested) &&
            std::find(wait->objects, wait->objects + wait->count, address) != wait->objects + wait->count) ++count;
    }
    return count;
}
void setDispatcherCancellation(DispatcherCancellation* cancellation) { currentCancellation = cancellation; }
void cancelDispatcherWaits(DispatcherCancellation& cancellation) {
    std::lock_guard lock(dispatcherMutex);
    cancellation.requested = true;
    dispatcherChanged.notify_all();
}
}

PPC_FUNC(__imp__KeSetEvent) {
    uint32_t address = ctx.r3.u32;
    std::lock_guard lock(dispatcherMutex);
    if (!validObject(base, address) || base[address] > 1) { ctx.r3.u64 = invalidParameter; return; }
    ctx.r3.u64 = read32(base, address + 4);
    write32(base, address + 4, 1);
    satisfyWaiters(base, address, ctx.r3.u32 == 0);
    dispatcherChanged.notify_all();
}
PPC_FUNC(__imp__KeResetEvent) {
    uint32_t address = ctx.r3.u32;
    std::lock_guard lock(dispatcherMutex);
    if (!validObject(base, address) || base[address] > 1) { ctx.r3.u64 = invalidParameter; return; }
    ctx.r3.u64 = read32(base, address + 4);
    write32(base, address + 4, 0);
}
PPC_FUNC(__imp__KeInitializeSemaphore) {
    uint32_t address = ctx.r3.u32;
    if ((address & 3) || !span(base, address, 20, true) || ctx.r4.s32 < 0 ||
        ctx.r5.s32 <= 0 || ctx.r4.s32 > ctx.r5.s32) { ctx.r3.u64 = invalidParameter; return; }
    std::lock_guard lock(dispatcherMutex);
    memset(base + address, 0, 20);
    base[address] = 5;
    write32(base, address + 4, ctx.r4.u32);
    write32(base, address + 8, address + 8);
    write32(base, address + 12, address + 8);
    write32(base, address + 16, ctx.r5.u32);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__KeReleaseSemaphore) {
    uint32_t address = ctx.r3.u32;
    std::unique_lock lock(dispatcherMutex);
    if (!validObject(base, address) || base[address] != 5 || ctx.r5.s32 <= 0) {
        ctx.r3.u64 = invalidParameter; return;
    }
    uint32_t previous = read32(base, address + 4);
    uint32_t adjustment = ctx.r5.u32; // r4 is a scheduling priority increment.
    if (adjustment > read32(base, address + 16) - previous) {
        lock.unlock();
        RaiseException(0xc0000047, 0, 0, nullptr); // STATUS_SEMAPHORE_LIMIT_EXCEEDED
        return;
    }
    write32(base, address + 4, previous + adjustment);
    satisfyWaiters(base, address, previous == 0);
    dispatcherChanged.notify_all();
    ctx.r3.u64 = previous;
}
PPC_FUNC(__imp__KeWaitForSingleObject) {
    uint32_t address = ctx.r3.u32;
    ctx.r3.u64 = waitObjects(base, &address, 1, 1, ctx.r6.u32, ctx.r7.u32);
}
PPC_FUNC(__imp__KeWaitForMultipleObjects) {
    uint32_t count = ctx.r3.u32, array = ctx.r4.u32;
    if (!count || count > 64 || (array & 3) || !span(base, array, count * 4)) {
        ctx.r3.u64 = invalidParameter; return;
    }
    uint32_t objects[64];
    for (uint32_t i = 0; i < count; ++i) objects[i] = read32(base, array + i * 4);
    // Wait records live on the native stack; the guest's optional wait block
    // storage is never retained or accessed after this call.
    ctx.r3.u64 = waitObjects(base, objects, count, ctx.r5.u32, ctx.r8.u32, ctx.r9.u32);
}
