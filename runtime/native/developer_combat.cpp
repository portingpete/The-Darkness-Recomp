#include "developer_combat.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
std::atomic<uint8_t*> activeBase{nullptr};
std::atomic<uint64_t> ammoPlayer{0};
std::atomic<uint64_t> hiddenPlayer{0};

bool combatProbeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_DEVELOPER_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

uint8_t* combatBase() {
    const auto& memory = DarkRecomp::Native::memory;
    auto* base = activeBase.load(std::memory_order_acquire);
    return memory && memory->base() == base ? base : nullptr;
}
}

namespace DarkRecomp::Native {
void configureDeveloperCombat(uint8_t* base, DeveloperPlayerHandles player, DeveloperCombatCheats cheats) noexcept {
    uint64_t identity = 0;
    if ((cheats.infiniteAmmo || cheats.enemiesIgnorePlayer) && memory && base == memory->base() &&
        player.server && player.actor && player.state && uint64_t(player.actor) + 640 <= PPC_MEMORY_SIZE) {
        activeBase.store(base, std::memory_order_release);
        identity = (uint64_t(player.actor) << 32) | player.state;
    }
    ammoPlayer.store(cheats.infiniteAmmo ? identity : 0, std::memory_order_release);
    hiddenPlayer.store(cheats.enemiesIgnorePlayer ? identity : 0, std::memory_order_release);
}
}

void PreserveDeveloperAmmoMidAsmHook(PPCRegister& weapon, PPCRegister& shooter, PPCRegister& rounds) {
    const auto player = ammoPlayer.load(std::memory_order_acquire);
    if (!player || !weapon.u32 || uint64_t(weapon.u32) + 398 > PPC_MEMORY_SIZE ||
        !shooter.u32 || shooter.u32 > INT16_MAX) return;
    auto* base = combatBase();
    if (!base) return;
    const auto actor = uint32_t(player >> 32);
    const auto server = PPC_LOAD_U32(actor + 636);
    if (!server || PPC_LOAD_U16(actor + 368) != shooter.u32 ||
        PPC_LOAD_U32(actor + 396) != uint32_t(player) || PPC_LOAD_U32(weapon.u32 + 16) != server) return;

    // The original shot still updates its sequence counter, applies effects,
    // and stores a magazine value. Preserve the amount loaded before the shot.
    const auto loaded = PPC_LOAD_U16(weapon.u32 + 396);
    if (combatProbeEnabled()) {
        static thread_local unsigned reported = 0;
        if (reported < 24) {
            ++reported;
            std::fprintf(stderr, "[DeveloperAmmo] shooter=%u weapon=%08x loaded=%d originalAfter=%d\n",
                shooter.u32, weapon.u32, int(int16_t(loaded)), int(int16_t(rounds.u32)));
        }
    }
    rounds.u64 = loaded;
}

bool IgnoreDeveloperPlayerTargetMidAsmHook(PPCRegister& observer, PPCRegister& candidate) {
    const auto player = hiddenPlayer.load(std::memory_order_acquire);
    if (!player || candidate.u32 != uint32_t(player >> 32) || !observer.u32 ||
        uint64_t(observer.u32) + 36 > PPC_MEMORY_SIZE) return false;
    auto* base = combatBase();
    if (!base || PPC_LOAD_U32(candidate.u32 + 396) != uint32_t(player)) return false;
    const auto server = PPC_LOAD_U32(candidate.u32 + 636);
    const auto observerActor = PPC_LOAD_U32(observer.u32 + 28);
    if (!server || !observerActor || observerActor == candidate.u32 ||
        PPC_LOAD_U32(observer.u32 + 32) != server) return false;

    // Use the predicate's existing rejection and register restore path.
    if (combatProbeEnabled()) {
        static thread_local unsigned reported = 0;
        if (reported < 24) {
            ++reported;
            std::fprintf(stderr, "[DeveloperTarget] observer=%08x observerActor=%08x player=%08x ineligible=1\n",
                observer.u32, observerActor, candidate.u32);
        }
    }
    return true;
}
