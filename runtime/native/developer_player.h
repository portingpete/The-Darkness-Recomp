#pragma once
#include <cstdint>
#include <string>

struct PPCContext;
union PPCRegister;
namespace DarkRecomp::Native {
struct DeveloperPlayerResult {
    bool hasActivePlayer = false;
    bool applied = false;
    std::string status;
};
DeveloperPlayerResult updateDeveloperPlayer(PPCContext& ctx, uint8_t* base,
    uint32_t client, float speed, bool invincible, bool enabled);
void resetDeveloperPlayer() noexcept;
}
void ApplyDeveloperHorizontalSpeedMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& forward, PPCRegister& sidestep);
void ApplyDeveloperForwardSpeedMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& forward);
