#include "graphics_settings.h"
#include "ppc_context.h"

// 82300818 evaluates CWObject_Light colors for illumination and lamp effects.
// At 82300AA0 f1 holds the periodic intensity multiplier (light modes 1..5).
// The original enabled check and color interpolation have already run; the
// authored intensity scaling and output stores still follow this hook.
// Using full intensity avoids a frozen dark phase. No light flags, timers,
// fades, script messages or saved state are modified, so disabling the option
// immediately resumes the original waveform at its current phase.
void ApplyLightFlickerReductionMidAsmHook(PPCRegister& multiplier) {
    if (DarkRecomp::Native::graphicsSettings().reduceLightFlicker)
        multiplier.f64 = 1.0;
}
