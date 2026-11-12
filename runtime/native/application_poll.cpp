#include "application_poll.h"
#include "ppc_recomp_shared.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>

using namespace DarkRecomp::Native;

namespace {
ApplicationPollMode applicationPollMode() {
    static const ApplicationPollMode mode = [] {
        const auto selected = applicationPollModeFrom(std::getenv("DARK_APP_POLL"));
        std::fprintf(stderr, "[ApplicationPoll] mode=%s (DARK_APP_POLL=0 restores the original loop)\n",
            selected == ApplicationPollMode::original ? "original" : "yield");
        return selected;
    }();
    return mode;
}
uint64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
}
uint64_t qpcFrequency() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return uint64_t(value.QuadPart);
    }();
    return frequency;
}
// Per-thread state: 825A4278 normally runs only on the application system
// thread, but 820E2E50 can reach it from another thread as well.
struct PollState {
    bool insideWait = false;
    uint64_t waits = 0, passes = 0, yields = 0, reportStart = 0;
    unsigned reports = 0;
};
thread_local PollState poll;

void report() {
    const auto now = qpcNow();
    if (!poll.reportStart) { poll.reportStart = now; return; }
    if (now - poll.reportStart < qpcFrequency() * 30 || poll.reports >= 32) return;
    ++poll.reports;
    std::fprintf(stderr, "[ApplicationPoll] tid=%lu mode=%s seconds=%.1f waits=%llu passes=%llu yieldedToReadyThreads=%llu\n",
        GetCurrentThreadId(), applicationPollMode() == ApplicationPollMode::original ? "original" : "yield",
        double(now - poll.reportStart) / double(qpcFrequency()), static_cast<unsigned long long>(poll.waits),
        static_cast<unsigned long long>(poll.passes), static_cast<unsigned long long>(poll.yields));
    poll.waits = poll.passes = poll.yields = 0;
    poll.reportStart = now;
}
}

extern "C" PPC_FUNC(__imp__sub_825A4278);
PPC_FUNC(sub_825A4278) {
    ++poll.waits;
    report();
    const bool outer = !poll.insideWait;
    poll.insideWait = true;
    __imp__sub_825A4278(ctx, base);
    if (outer) poll.insideWait = false;
}

extern "C" PPC_FUNC(__imp__sub_822360B8);
PPC_FUNC(sub_822360B8) {
    const bool pass = applicationPollPass(poll.insideWait, uint32_t(ctx.lr));
    __imp__sub_822360B8(ctx, base);
    if (!pass) return;
    ++poll.passes;
    if (applicationPollMode() == ApplicationPollMode::yield && SwitchToThread()) ++poll.yields;
}
