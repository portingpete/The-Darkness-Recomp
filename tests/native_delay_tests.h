#pragma once

// Exercise the actual PPC import and original SDK wrapper, including APC
// reentrancy. Timing assertions enforce deadlines, not host performance limits.
namespace {
using DelayClock = std::chrono::steady_clock;
uint32_t delayCall(PPCContext& ctx, uint32_t storage, int64_t ticks, bool alertable) {
    auto* base = memory->base();
    PPC_STORE_U64(storage, uint64_t(ticks));
    ctx.r3.u64 = 1;
    ctx.r4.u64 = alertable;
    ctx.r5.u64 = storage;
    __imp__KeDelayExecutionThread(ctx, base);
    return ctx.r3.u32;
}
uint64_t delaySystemTime() {
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}
struct DelayApc {
    PPCContext* ctx;
    uint32_t storage;
    unsigned calls = 0;
    bool nested = false;
    uint32_t nestedStatus = 0xffffffff;
    double nestedSeconds = 0;
};
void CALLBACK delayApc(ULONG_PTR raw) {
    auto& probe = *reinterpret_cast<DelayApc*>(raw);
    ++probe.calls;
    if (probe.nested) {
        PPCContext nested = *probe.ctx;
        const auto begin = DelayClock::now();
        probe.nestedStatus = delayCall(nested, probe.storage + 8, -100000, false);
        probe.nestedSeconds = std::chrono::duration<double>(DelayClock::now() - begin).count();
    }
}
void testNativeDelay(PPCContext& ctx) {
    auto* base = memory->base();
    const uint32_t storage = memory->allocate(4096);
    check(storage != 0, "Delay fixture allocation failed");
    ctx.r5.u64 = 0;
    __imp__KeDelayExecutionThread(ctx, base);
    check(ctx.r3.u32 == 0xc000000d, "Delay accepted a null interval");
    check(delayCall(ctx, storage, 0, false) == 0, "Zero delay failed");
    check(delayCall(ctx, storage, 1, false) == 0, "Expired absolute delay failed");
    auto begin = DelayClock::now();
    check(delayCall(ctx, storage, -100000, false) == 0, "Relative delay failed");
    check(std::chrono::duration<double>(DelayClock::now() - begin).count() >= 0.0095,
        "Relative delay completed before its interval");
    const uint64_t deadline = delaySystemTime() + 100000;
    check(delayCall(ctx, storage, int64_t(deadline), false) == 0, "Absolute delay failed");
    check(delaySystemTime() >= deadline, "Absolute delay completed before its deadline");

    DelayApc probe{&ctx, storage};
    auto queue = [&] {
        check(QueueUserAPC(delayApc, GetCurrentThread(), reinterpret_cast<ULONG_PTR>(&probe)) != 0,
            "Could not queue delay APC");
    };
    queue();
    check(delayCall(ctx, storage, -10000, false) == 0 && probe.calls == 0,
        "Non-alertable delay delivered an APC");
    check(delayCall(ctx, storage, 0, true) == 0xc0 && probe.calls == 1,
        "Zero alertable delay failed to deliver queued APC/status");
    probe.nested = true;
    queue();
    check(delayCall(ctx, storage, -10000000, true) == 0xc0 && probe.calls == 2,
        "Alertable timer delay did not return APC status");
    check(probe.nestedStatus == 0 && probe.nestedSeconds >= 0.0095,
        "Reentrant APC delay consumed the outer timer or returned early");
    probe.nested = false;
    // Reusing the cached timer after interruption must rearm it rather than
    // consume the signal from the nested wait.
    begin = DelayClock::now();
    check(delayCall(ctx, storage, -100000, false) == 0, "Rearmed delay failed");
    check(std::chrono::duration<double>(DelayClock::now() - begin).count() >= 0.0095,
        "Rearmed delay consumed a stale timer signal");
    queue();
    check(delayCall(ctx, storage, INT64_MIN, true) == 0xc0 && probe.calls == 3,
        "Longest relative delay lost APC delivery");
    queue();
    ctx.r3.u64 = 0xffffffffu;
    ctx.r4.u64 = 1;
    sub_828AC000(ctx, base);
    check(ctx.r3.u32 == WAIT_IO_COMPLETION && probe.calls == 4,
        "Original SDK infinite sleep lost APC status mapping");
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 0;
    const uint64_t nonvolatile = ctx.r31.u64;
    sub_828AC000(ctx, base);
    check(ctx.r3.u32 == 0 && ctx.r31.u64 == nonvolatile,
        "Original SDK zero sleep changed result/nonvolatile register");

    // Print an in-process comparison without making speed a correctness gate.
    double totals[2]{};
    for (unsigned i = 0; i < 120; ++i) {
        for (unsigned j = 0; j < 2; ++j) {
            const unsigned mode = (i + j) & 1;
            begin = DelayClock::now();
            if (mode) check(delayCall(ctx, storage, -10000, false) == 0, "Timed delay failed");
            else SleepEx(1, FALSE);
            if (i >= 20) totals[mode] += std::chrono::duration<double, std::milli>(DelayClock::now() - begin).count();
        }
    }
    printf("Delay 1 ms means (100 alternating samples): SleepEx=%.4f ms, import=%.4f ms\n",
        totals[0] / 100, totals[1] / 100);
    check(memory->release(storage), "Delay fixture cleanup failed");
    puts("Native delay deadlines, yields, alertability, reentrancy and original SDK ABI passed.");
}
}
