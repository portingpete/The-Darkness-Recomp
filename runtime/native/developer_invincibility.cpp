#include "developer_invincibility.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <atomic>

namespace {
std::atomic<uint8_t*> activeBase{nullptr};
std::atomic<uint64_t> activePlayer{0};
}

namespace DarkRecomp::Native {
void configureDeveloperInvincibility(uint8_t* base, uint32_t actor, uint32_t state,
                                    bool invincible, bool enabled) noexcept {
    if (!enabled || !invincible || !memory || base != memory->base() || !actor || !state ||
        uint64_t(actor) + 400 > PPC_MEMORY_SIZE || uint64_t(state) + 10244 > PPC_MEMORY_SIZE) {
        activePlayer.store(0, std::memory_order_release);
        return;
    }
    activeBase.store(base, std::memory_order_release);
    activePlayer.store((uint64_t(actor) << 32) | state, std::memory_order_release);
}
}

void ApplyDeveloperInvincibilityMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& flags) {
    const auto player = activePlayer.load(std::memory_order_acquire);
    if (!player || uint32_t(player >> 32) != actor.u32 || uint32_t(player) != state.u32 ||
        !DarkRecomp::Native::memory || activeBase.load(std::memory_order_acquire) != DarkRecomp::Native::memory->base()) return;
    auto* base = DarkRecomp::Native::memory->base();
    if (PPC_LOAD_U32(actor.u32 + 396) != state.u32) return;
    // The next original instruction isolates bit 0x200 for its normal god-mode
    // predicate. Override that register only, retaining original damage/death
    // behavior and leaving replicated/serialized actor flags untouched.
    flags.u64 |= 0x200;
}
