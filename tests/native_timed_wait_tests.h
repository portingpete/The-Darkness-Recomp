#pragma once

#include "runtime/native/native_timed_wait.h"
#include <chrono>
#include <thread>

namespace {
struct TimedWaitApc {
    HANDLE event;
    unsigned calls = 0;
    DWORD nestedResult = WAIT_FAILED;
};
void CALLBACK timedWaitApc(ULONG_PTR raw) {
    auto& probe = *reinterpret_cast<TimedWaitApc*>(raw);
    ++probe.calls;
    const int64_t nested = -50000;
    probe.nestedResult = DarkRecomp::Native::nativeTimedWait(probe.event, FALSE, &nested);
}
void testNativeTimedWait(PPCContext& ctx) {
    using DarkRecomp::Native::nativeTimedWait;
    auto* base = memory->base();
    const uint32_t storage = memory->allocate(4096);
    check(storage != 0, "Timed wait fixture allocation failed");
    ctx.r3.u64 = storage; ctx.r4.u64 = 0; ctx.r5.u64 = 1; ctx.r6.u64 = 0;
    __imp__NtCreateEvent(ctx, base);
    check(ctx.r3.u32 == 0, "Timed wait event creation failed");
    const uint32_t event = memory->read32(storage);
    auto wait = [&](int64_t ticks, bool alertable = false, bool infinite = false) {
        PPC_STORE_U64(storage + 8, uint64_t(ticks));
        ctx.r3.u64 = event; ctx.r4.u64 = 0; ctx.r5.u64 = alertable;
        ctx.r6.u64 = infinite ? 0 : storage + 8;
        __imp__NtWaitForSingleObjectEx(ctx, base);
        return ctx.r3.u32;
    };
    auto signal = [&] {
        ctx.r3.u64 = event; ctx.r4.u64 = 0;
        __imp__NtSetEvent(ctx, base);
        check(ctx.r3.u32 == 0, "Timed wait signal failed");
    };
    check(wait(0) == WAIT_TIMEOUT && wait(1) == WAIT_TIMEOUT, "Expired wait did not time out");
    signal();
    check(wait(1) == 0 && wait(0) == WAIT_TIMEOUT, "Signaled object lost precedence or auto-reset semantics");
    signal();
    check(wait(0, false, true) == 0, "Infinite object wait failed");
    auto begin = std::chrono::steady_clock::now();
    check(wait(-50000) == WAIT_TIMEOUT, "Relative object wait status changed");
    check(std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count() >= .0045,
          "Relative object wait returned before its deadline");
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const uint64_t deadline = ((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime) + 50000;
    check(wait(int64_t(deadline)) == WAIT_TIMEOUT, "Absolute object wait status changed");
    GetSystemTimePreciseAsFileTime(&now);
    check(((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime) >= deadline,
          "Absolute object wait returned before its deadline");

    HANDLE rawEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(rawEvent != nullptr, "Host wait fixture creation failed");
    TimedWaitApc probe{rawEvent};
    check(QueueUserAPC(timedWaitApc, GetCurrentThread(), reinterpret_cast<ULONG_PTR>(&probe)) != 0,
          "Timed wait APC queue failed");
    check(wait(-50000) == WAIT_TIMEOUT && probe.calls == 0, "Non-alertable wait delivered an APC");
    check(wait(-10000000, true) == WAIT_IO_COMPLETION && probe.calls == 1 && probe.nestedResult == WAIT_TIMEOUT,
          "Object wait lost APC status or nested timeout");
    // Deliver another APC after the outer timer has actually been leased.
    HANDLE caller = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
    check(caller != nullptr, "Timed wait caller handle failed");
    std::thread apcSender([&] {
        Sleep(10);
        QueueUserAPC(timedWaitApc, caller, reinterpret_cast<ULONG_PTR>(&probe));
    });
    const int64_t longWait = -10000000;
    const DWORD interrupted = nativeTimedWait(rawEvent, TRUE, &longWait);
    apcSender.join();
    CloseHandle(caller);
    check(interrupted == WAIT_IO_COMPLETION && probe.calls == 2 && probe.nestedResult == WAIT_TIMEOUT,
          "Nested APC wait reused the active outer timer");
    std::thread sender([&] { Sleep(10); SetEvent(rawEvent); });
    const DWORD signaled = nativeTimedWait(rawEvent, FALSE, &longWait);
    sender.join();
    check(signaled == 0, "Object signal could not interrupt a timed wait");
    const int64_t shortWait = -50000;
    begin = std::chrono::steady_clock::now();
    check(nativeTimedWait(rawEvent, FALSE, &shortWait) == WAIT_TIMEOUT,
          "Rearmed object wait consumed an old signal");
    check(std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count() >= .0045,
          "Rearmed timer returned early");
    // Host load is not a correctness assertion; retain the comparison as evidence.
    double totals[2]{};
    for (unsigned i=0;i<30;++i) for (unsigned j=0;j<2;++j) {
        const unsigned mode=(i+j)&1;
        begin=std::chrono::steady_clock::now();
        const DWORD result=mode ? wait(-50000) : WaitForSingleObjectEx(rawEvent,5,FALSE);
        check(result==WAIT_TIMEOUT,"5 ms wait comparison did not time out");
        totals[mode]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    }
    printf("Object wait 5 ms means (30 alternating samples): Win32=%.4f ms, import=%.4f ms\n",
           totals[0]/30,totals[1]/30);
    CloseHandle(rawEvent);
    ctx.r3.u64=event; __imp__NtClose(ctx,base);
    check(wait(0)==0xc0000008,"Timed wait accepted a closed handle");
    check(memory->release(storage),"Timed wait fixture cleanup failed");
    puts("Native object waits preserve deadlines, signals, auto-reset, APCs and reentrancy.");
}
}
