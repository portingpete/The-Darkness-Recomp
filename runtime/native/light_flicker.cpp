#include "graphics_settings.h"
#include "ppc_recomp_shared.h"

extern "C" PPC_FUNC(__imp__sub_822FDF20);

// CWObject_Light's color evaluator calls this shared waveform utility at
// 82300A90, after its enabled check and color interpolation. r6 selects modes
// 1..5 minus one. Mode 4 is a one-shot burnout: 822FDD18 becomes permanently
// zero after one second. Destruction selects that mode without clearing the
// enabled bit, so forcing its result to one kept broken lamps illuminated.
// Capture the caller and mode before the original call can clobber arguments.
// Only periodic light modes 1/2/3/5 become steady; burnout, other callers and
// original register/state side effects retain the authored behavior.
PPC_FUNC(sub_822FDF20) {
    const bool steady = uint32_t(ctx.lr) == 0x82300A94 && ctx.r6.u32 < 5 &&
        ctx.r6.u32 != 3 && DarkRecomp::Native::graphicsSettings().reduceLightFlicker;
    __imp__sub_822FDF20(ctx, base);
    if (steady) ctx.f1.f64 = 1.0;
}
