#pragma once
#include <cstdint>

union PPCRegister;
namespace DarkRecomp::Native {
// Resolve the active server player on the engine thread before configuring.
// Only original movement/collision registers are overridden; no cheat mode is saved.
void configureDeveloperNoclip(uint8_t* base, uint32_t actor, uint32_t state,
                              bool noclip, bool enabled) noexcept;
}
void ApplyDeveloperNoclipMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& mode);
void ApplyDeveloperNoclipStateMidAsmHook(PPCRegister& state, PPCRegister& mode);
void ApplyDeveloperNoclipCollisionMidAsmHook(PPCRegister& descriptor, PPCRegister& active);
