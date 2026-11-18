#pragma once
#include "developer_player_lookup.h"

union PPCRegister;

namespace DarkRecomp::Native {
struct DeveloperCombatCheats {
    bool infiniteAmmo = false;
    bool enemiesIgnorePlayer = false;
    bool operator==(const DeveloperCombatCheats&) const = default;
};

void configureDeveloperCombat(uint8_t* base, DeveloperPlayerHandles player, DeveloperCombatCheats cheats) noexcept;
}

void PreserveDeveloperAmmoMidAsmHook(PPCRegister& weapon, PPCRegister& shooter, PPCRegister& rounds);
bool IgnoreDeveloperPlayerTargetMidAsmHook(PPCRegister& observer, PPCRegister& candidate);
