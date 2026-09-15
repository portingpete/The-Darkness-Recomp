#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <cstdio>

namespace DarkRecomp::Native {
int runGuestWithEntry(PPCContext& ctx, uint8_t* base, PPCFunc* entry) {
    __try {
        entry(ctx, base);
        fprintf(stderr, "[STOP] Game entry returned before a playable frame.\n");
        return 4;
    } __except (exceptionFilter(GetExceptionInformation())) {
        return 3;
    }
}
int runGuest(PPCContext& ctx, uint8_t* base) { return runGuestWithEntry(ctx, base, _xstart); }
}
