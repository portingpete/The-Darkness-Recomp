#pragma once
#include <cstdint>
union PPCRegister;
namespace DarkRecomp::Native {
bool movementProbeEnabled() noexcept;
float movementHorizontalSpeedScale(uint32_t actor, uint32_t state) noexcept;
// Only the current local player's retail prediction callback may opt a clone
// into the same controller as the authoritative body. Nesting restores scope.
class MovementPredictionScope {
public:
    MovementPredictionScope(uint8_t* base, uint32_t client, uint32_t actor, uint32_t state) noexcept;
    ~MovementPredictionScope();
    MovementPredictionScope(const MovementPredictionScope&) = delete;
    MovementPredictionScope& operator=(const MovementPredictionScope&) = delete;
private:
    uint32_t actor_, state_, world_;
    uint64_t selected_, revision_;
};
void limitAlternativeFall(uint32_t actor, uint32_t state, uint32_t server,
    uint32_t force, float fraction, bool grounded) noexcept;
void configureMovementPlayer(uint8_t* base, uint32_t actor, uint32_t state,
                             float developerSpeed = 1, bool noclip = false, uint32_t client = 0, uint32_t server = 0) noexcept;
}
void ShapeAlternativeMovementWishMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& stack, PPCRegister& mode, PPCRegister& command,
    PPCRegister& forward, PPCRegister& side, PPCRegister& x, PPCRegister& y);
void PrepareAlternativeMovementMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& server, PPCRegister& stack, PPCRegister& grounded,
    PPCRegister& platformX, PPCRegister& platformY);
void ApplyAlternativeMovementMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& server,
    PPCRegister& stack, PPCRegister& grounded);
void ApplyAlternativeJumpMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& server, PPCRegister& impulse);
void StabilizeAlternativeMovementCameraMidAsmHook(PPCRegister& client, PPCRegister& actor,
    PPCRegister& state, PPCRegister& matrix);
