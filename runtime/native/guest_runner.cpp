#include "runtime.h"
#include "stall_profiler.h"
#include "ppc_recomp_shared.h"
#include <cstdio>

namespace DarkRecomp::Native {
// Keep objects with destructors outside the function containing SEH.
static int runGuestWithEntrySeh(PPCContext& ctx, uint8_t* base, PPCFunc* entry) {
    __try {
        entry(ctx, base);
        fprintf(stderr, "[STOP] Game entry returned before a playable frame.\n");
        return 4;
    } __except (exceptionFilter(GetExceptionInformation())) {
        return 3;
    }
}
int runGuestWithEntry(PPCContext& ctx, uint8_t* base, PPCFunc* entry) {
    StallProfiler::Scope profile(StallProfiler::Section::Guest, "guest-entry",
        entry == _xstart ? 0x828AA3E8u : ctx.lastFunction, uint32_t(ctx.lr));
    return runGuestWithEntrySeh(ctx, base, entry);
}
int runGuest(PPCContext& ctx, uint8_t* base) { return runGuestWithEntry(ctx, base, _xstart); }
}
