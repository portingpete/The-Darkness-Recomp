#pragma once
#include "dispatcher_wake_tests.h"
#include "dispatcher_timeout_tests.h"
#include "dispatcher_wait_all_tests.h"

static uint32_t releaseSemaphoreException(PPCContext& ctx, uint8_t* base) {
    __try { __imp__KeReleaseSemaphore(ctx, base); return 0; }
    __except (GetExceptionCode() == 0xc0000047 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return GetExceptionCode();
    }
}

static void testDispatcher() {
    auto* base = memory->base();
    uint32_t scratch = memory->allocate(4096);
    check(scratch != 0, "Dispatcher fixture allocation failed");
    const uint32_t first = scratch, second = scratch + 32, sem = scratch + 64;
    const uint32_t array = scratch + 96, timeout = scratch + 128;
    auto event = [&](uint32_t at, uint8_t type, uint32_t state) {
        memset(base + at, 0, 16);
        base[at] = type;
        memory->write32(at + 4, state);
        memory->write32(at + 8, at + 8);
        memory->write32(at + 12, at + 8);
    };
    auto ticks = [&](int64_t value) {
        uint64_t raw = _byteswap_uint64(uint64_t(value));
        memcpy(base + timeout, &raw, 8);
    };
    auto single = [&](uint32_t address, uint32_t time, uint32_t alertable = 0) {
        PPCContext ctx{};
        ctx.r3.u64 = address; ctx.r6.u64 = alertable; ctx.r7.u64 = time;
        __imp__KeWaitForSingleObject(ctx, base);
        return ctx.r3.u32;
    };
    auto multiple = [&](uint32_t kind, uint32_t count = 2, uint32_t ptr = 0) {
        PPCContext ctx{};
        ctx.r3.u64 = count; ctx.r4.u64 = ptr ? ptr : array;
        ctx.r5.u64 = kind; ctx.r9.u64 = timeout;
        __imp__KeWaitForMultipleObjects(ctx, base);
        return ctx.r3.u32;
    };
    auto signal = [&](uint32_t at) {
        PPCContext ctx{}; ctx.r3.u64 = at;
        __imp__KeSetEvent(ctx, base);
    };
    ticks(0);
    event(first, 1, 0); event(second, 0, 1);
    memory->write32(array, first); memory->write32(array + 4, second);
    check(multiple(1) == 1 && memory->read32(second + 4) == 1,
          "Wait-any lost index 1 or consumed a manual-reset event");
    signal(first);
    check(multiple(1) == 0 && memory->read32(first + 4) == 0,
          "Wait-any did not select/consume the first ready auto-reset event");
    event(first, 1, 1); event(second, 1, 0);
    check(multiple(0) == 0x102 && memory->read32(first + 4) == 1,
          "Wait-all partially consumed objects on timeout");
    signal(second);
    check(multiple(0) == 0 && memory->read32(first + 4) == 0 && memory->read32(second + 4) == 0,
          "Wait-all did not consume both auto-reset events");
    PPCContext ctx{};
    ctx.r3.u64 = sem; ctx.r4.u64 = 0; ctx.r5.u64 = 3;
    __imp__KeInitializeSemaphore(ctx, base);
    check(base[sem] == 5 && memory->read32(sem + 16) == 3, "Semaphore byte header/limit wrong");
    ctx.r3.u64 = sem; ctx.r4.u64 = 99; ctx.r5.u64 = 2; ctx.r6.u64 = 0;
    __imp__KeReleaseSemaphore(ctx, base);
    check(ctx.r3.u32 == 0 && memory->read32(sem + 4) == 2, "Semaphore used priority instead of adjustment");
    check(single(sem, timeout) == 0 && memory->read32(sem + 4) == 1, "Wait did not decrement semaphore");
    ctx.r3.u64 = sem; ctx.r4.u64 = 0; ctx.r5.u64 = 3;
    check(releaseSemaphoreException(ctx, base) == 0xc0000047 && memory->read32(sem + 4) == 1,
          "Semaphore overflow was clamped or changed state");
    memory->write32(array + 4, sem);
    check(multiple(0) == 0x102 && memory->read32(sem + 4) == 1,
          "Wait-all prematurely decremented a semaphore");
    signal(first);
    check(multiple(0) == 0 && memory->read32(sem + 4) == 0, "Wait-all semaphore acquisition failed");
    check(single(first, timeout, 1) == 0xc00000bb, "Alertable wait falsely reported supported");
    check(single(0, timeout) == 0xc000000d && single(0xfffffffcu, timeout) == 0xc000000d,
          "Wait accepted null or wrapping object");
    check(single(first, 0xfffffffcu) == 0xc000000d, "Wait accepted wrapping timeout span");
    check(multiple(2) == 0xc000000d && multiple(0, 0) == 0xc000000d &&
          multiple(1, 65) == 0xc000000d && multiple(1, 2, 0xfffffffcu) == 0xc000000d,
          "Wait accepted bad type/count/array span");
    memory->write32(array + 4, first);
    check(multiple(0) == 0xc000000d, "Wait accepted duplicate objects");
    event(second, 9, 0);
    check(single(second, timeout) == 0xc000000d, "Wait accepted unsupported dispatcher object type");
    ticks(-400000); // 40 ms, independent of wall-clock changes.
    uint64_t start = GetTickCount64();
    check(single(first, timeout) == 0x102 && GetTickCount64() - start >= 30,
          "Relative timeout returned immediately");
    ticks(1);
    check(single(first, timeout) == 0x102, "Expired absolute timeout did not return");
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ticks(int64_t((uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime) + 400000);
    start = GetTickCount64();
    check(single(first, timeout) == 0x102 && GetTickCount64() - start >= 25,
          "Future absolute timeout returned immediately");
    // Infinite single wait must report success after a real other-thread signal.
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto waiter = std::async(std::launch::async, [&] {
        entered.set_value();
        return single(first, 0);
    });
    ready.wait();
    check(waiter.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout,
          "Unsignaled infinite wait returned");
    signal(first);
    check(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready && waiter.get() == 0 &&
          memory->read32(first + 4) == 0, "Awakened infinite wait lost success or auto-reset");
    event(second, 0, 0);
    memory->write32(array + 4, second);
    auto multiWaiter = std::async(std::launch::async, [&] {
        PPCContext waitCtx{};
        waitCtx.r3.u64 = 2; waitCtx.r4.u64 = array; waitCtx.r5.u64 = 1;
        waitCtx.r6.u64 = 3; waitCtx.r7.u64 = 1; waitCtx.r10.u64 = scratch + 160;
        __imp__KeWaitForMultipleObjects(waitCtx, base);
        return waitCtx.r3.u32;
    });
    check(multiWaiter.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout,
          "Unsignaled infinite multiple wait returned");
    signal(second);
    check(multiWaiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
          multiWaiter.get() == 1 && memory->read32(second + 4) == 1,
          "Original audio wait ABI lost the selected index or manual-reset state");
    // Host cancellation must unwind, leaving guest signal state and r3 untouched.
    DispatcherCancellation cancellation;
    auto cancelled = std::async(std::launch::async, [&] {
        setDispatcherCancellation(&cancellation);
        PPCContext waitCtx{};
        waitCtx.r3.u64 = first;
        bool caught = false;
        try { __imp__KeWaitForSingleObject(waitCtx, base); }
        catch (const DispatcherWaitCancelled&) { caught = true; }
        setDispatcherCancellation(nullptr);
        return caught && waitCtx.r3.u32 == first;
    });
    check(cancelled.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout,
          "Cancellation fixture did not block");
    cancelDispatcherWaits(cancellation);
    signal(first); // A cancelled waiter must not consume a later signal.
    check(cancelled.wait_for(std::chrono::seconds(2)) == std::future_status::ready && cancelled.get() &&
          memory->read32(first + 4) == 1, "Host cancellation fabricated a guest completion or consumed a later signal");
    memory->release(scratch);
    testDispatcherWakeCommit();
    testDispatcherTimeoutOwnership();
    testDispatcherWaitAllWake();
    puts("Dispatcher: any/all, byte headers, semaphore overflow, timeouts, wake and cancellation verified.");
}
