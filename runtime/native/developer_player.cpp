#include "developer_player.h"
#include "developer_invincibility.h"
#include "developer_noclip.h"
#include "developer_player_lookup.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
std::atomic<uint8_t*> activeBase{nullptr};
std::atomic<uint64_t> activePlayer{0};
std::atomic<float> activeSpeed{1};

float movementMultiplier(const PPCRegister& actor, const PPCRegister& state) {
    const auto player = activePlayer.load(std::memory_order_acquire);
    if (!player || uint32_t(player >> 32) != actor.u32 || uint32_t(player) != state.u32 ||
        !DarkRecomp::Native::memory || activeBase.load(std::memory_order_acquire) != DarkRecomp::Native::memory->base()) return 1;
    auto* base = DarkRecomp::Native::memory->base();
    if (PPC_LOAD_U32(actor.u32 + 396) != state.u32) return 1;
    return activeSpeed.load(std::memory_order_acquire);
}

bool probeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_DEVELOPER_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
}

namespace DarkRecomp::Native {
void resetDeveloperPlayer() noexcept {
    activePlayer.store(0, std::memory_order_release);
    configureDeveloperInvincibility(nullptr, 0, 0, false, false);
    configureDeveloperNoclip(nullptr, 0, 0, false, false);
}

DeveloperPlayerResult updateDeveloperPlayer(PPCContext& ctx, uint8_t* base, uint32_t client,
    float speed, bool invincible, bool enabled, bool noclip) {
    const auto player = resolveDeveloperPlayer(ctx, base, client);
    const bool available = player.actor && player.state;
    const bool validSpeed = std::isfinite(speed) && speed >= 0.25f && speed <= 4;
    const bool apply = available && enabled && validSpeed;
    configureDeveloperInvincibility(base, player.actor, player.state, invincible, apply);
    configureDeveloperNoclip(base, player.actor, player.state, noclip, apply);
    activePlayer.store(0, std::memory_order_release);
    if (apply && speed != 1) {
        activeBase.store(base, std::memory_order_release);
        activeSpeed.store(speed, std::memory_order_release);
        activePlayer.store((uint64_t(player.actor) << 32) | player.state, std::memory_order_release);
    }

    struct Previous {
        uint8_t* base = nullptr;
        uint32_t actor = ~0u, component = 0;
        float speed = 0;
        bool invincible = false, noclip = false, enabled = false;
        unsigned probes = 0;
    };
    thread_local Previous previous;
    DeveloperPlayerResult result{available, apply, {}};
    if (previous.base != base || previous.actor != player.actor || previous.component != player.state ||
        previous.speed != speed || previous.invincible != invincible || previous.noclip != noclip || previous.enabled != enabled) {
        char status[160];
        if (!available) std::snprintf(status, sizeof(status), "Waiting for an active player.");
        else if (!validSpeed) std::snprintf(status, sizeof(status), "Player speed must be between 0.25x and 4x.");
        else if (!enabled) std::snprintf(status, sizeof(status), "Player controls ready.");
        else std::snprintf(status, sizeof(status), "Player speed %.2gx; invincibility %s; noclip %s.",
                           speed, invincible ? "on" : "off", noclip ? "on" : "off");
        result.status = status;
        if (probeEnabled() && previous.probes++ < 40) {
            const auto forward = available ? std::bit_cast<float>(PPC_LOAD_U32(player.state + 10024)) : 0;
            const auto side = available ? std::bit_cast<float>(PPC_LOAD_U32(player.state + 10028)) : 0;
            const auto flags = available ? PPC_LOAD_U32(player.state + 10240) : 0;
            std::fprintf(stderr, "[DeveloperPlayer] base=%p client=%08x server=%08x actor=%08x state=%08x forward=%g side=%g god=%u multiplier=%g enabled=%u noclip=%u\n",
                static_cast<void*>(base), client, player.server, player.actor, player.state,
                forward, side, (flags & 0x200) != 0, speed, enabled, noclip);
        }
        previous.base = base; previous.actor = player.actor; previous.component = player.state;
        previous.speed = speed; previous.invincible = invincible; previous.enabled = enabled;
        previous.noclip = noclip;
    }
    return result;
}
}

// Original 82133DD8 consumes SPEED_FORWARD, SPEED_SIDESTEP and walking speed
// directly from its state. Multiply the loaded movement values once, before
// basis-vector construction. Jump/up speed, input magnitude, elapsed time and
// serialized/replicated state retain their original values.
void ApplyDeveloperHorizontalSpeedMidAsmHook(PPCRegister& actor, PPCRegister& state,
    PPCRegister& forward, PPCRegister& sidestep) {
    const auto multiplier = movementMultiplier(actor, state);
    if (multiplier == 1) return;
    forward.f64 = float(float(forward.f64) * multiplier);
    sidestep.f64 = float(float(sidestep.f64) * multiplier);
}

void ApplyDeveloperForwardSpeedMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& forward) {
    const auto multiplier = movementMultiplier(actor, state);
    if (multiplier != 1) forward.f64 = float(float(forward.f64) * multiplier);
}
