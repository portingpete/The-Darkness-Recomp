#include "developer_noclip.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
std::atomic<uint8_t*> activeBase{nullptr};
std::atomic<uint64_t> activePlayer{0};
std::atomic<uint64_t> pairCalls{0}, pairMatches{0}, stateCalls{0}, stateMatches{0}, probeUpdates{0};
std::atomic<uint64_t> collisionCalls{0}, collisionMatches{0};

bool probeEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_DEVELOPER_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

void probeHook(bool stateOnly, uint64_t player, uint32_t actor, uint32_t state,
               uint32_t mode, bool matched) noexcept {
    if (!player || !probeEnabled()) return;
    auto& calls = stateOnly ? stateCalls : pairCalls;
    auto& matches = stateOnly ? stateMatches : pairMatches;
    const auto count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto accepted = matched ? matches.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (count <= 4 || (matched && accepted <= 4)) {
        const auto savedMath = _mm_getcsr();
        std::fprintf(stderr, "[NoclipProbe] hook=%s actor=%08X state=%08X originalMode=%u selectedActor=%08X selectedState=%08X matched=%u\n",
                     stateOnly ? "state" : "pair", actor, state, mode,
                     uint32_t(player >> 32), uint32_t(player), unsigned(matched));
        _mm_setcsr(savedMath);
    }
}

bool selectedPlayer(uint64_t player, uint32_t actor, uint32_t state) noexcept {
    if (!player || uint32_t(player >> 32) != actor || uint32_t(player) != state ||
        !DarkRecomp::Native::memory ||
        activeBase.load(std::memory_order_acquire) != DarkRecomp::Native::memory->base()) return false;
    auto* base = DarkRecomp::Native::memory->base();
    if (PPC_LOAD_U32(actor + 396) != state) return false;
    // The original noclip command82133C58 refuses this separate flying mode.
    return (PPC_LOAD_U32(actor + 364) & 0x00100000) == 0;
}
}

namespace DarkRecomp::Native {
void configureDeveloperNoclip(uint8_t* base, uint32_t actor, uint32_t state,
                              bool noclip, bool enabled) noexcept {
    if (!enabled || !noclip || !memory || base != memory->base() || !actor || !state ||
        uint64_t(actor) + 400 > PPC_MEMORY_SIZE || uint64_t(state) + 10244 > PPC_MEMORY_SIZE) {
        activePlayer.store(0, std::memory_order_release);
        return;
    }
    activeBase.store(base, std::memory_order_release);
    activePlayer.store((uint64_t(actor) << 32) | state, std::memory_order_release);
    const auto update = probeEnabled() ? probeUpdates.fetch_add(1, std::memory_order_relaxed) : 3840;
    if (update < 3840 && update % 120 == 0)
        std::fprintf(stderr, "[NoclipProbe] configured actor=%08X state=%08X pairCalls=%llu pairMatches=%llu stateCalls=%llu stateMatches=%llu collisionCalls=%llu collisionMatches=%llu\n",
                     actor, state, static_cast<unsigned long long>(pairCalls.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(pairMatches.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(stateCalls.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(stateMatches.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(collisionCalls.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(collisionMatches.load(std::memory_order_relaxed)));
}
}

void ApplyDeveloperNoclipMidAsmHook(PPCRegister& actor, PPCRegister& state, PPCRegister& mode) {
    const auto player = activePlayer.load(std::memory_order_acquire);
    const bool matched = selectedPlayer(player, actor.u32, state.u32);
    probeHook(false, player, actor.u32, state.u32, mode.u32, matched);
    if (!matched) return;
    // Original mode4 selects free movement in82133DD8/82175F58. Its caller
    //82136B10 also uses this mode to omit gravity from the returned displacement.
    mode.u64 = 4;
}

void ApplyDeveloperNoclipStateMidAsmHook(PPCRegister& state, PPCRegister& mode) {
    const auto player = activePlayer.load(std::memory_order_acquire);
    const auto actor = uint32_t(player >> 32);
    const bool selected = selectedPlayer(player, actor, state.u32);
    if (!selected) { probeHook(true, player, actor, state.u32, mode.u32, false); return; }
    auto* base = DarkRecomp::Native::memory->base();
    //821720A8 receives only the character state. Match its original owner link
    // before selecting its existing noclip return ahead of collision queries.
    const bool matched = PPC_LOAD_U32(state.u32 + 16) == actor;
    probeHook(true, player, actor, state.u32, mode.u32, matched);
    if (matched) mode.u64 = 4;
}

void ApplyDeveloperNoclipCollisionMidAsmHook(PPCRegister& descriptor, PPCRegister& active) {
    const auto player = activePlayer.load(std::memory_order_acquire);
    const auto actor = uint32_t(player >> 32);
    const auto state = uint32_t(player);
    // The server's outer movement sweep has its own descriptor gate, after the
    // character returns a displacement. Original mode4 clears descriptor+56
    // in821334C0. Select its existing82472DF0 early return only for this player,
    // without changing the resident physics descriptor or saved player flags.
    const bool selected = descriptor.u32 == uint64_t(actor) + 264 && selectedPlayer(player, actor, state);
    auto* base = selected ? DarkRecomp::Native::memory->base() : nullptr;
    const bool matched = selected && PPC_LOAD_U32(state + 16) == actor;
    if (player && probeEnabled()) {
        const auto count = collisionCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto accepted = matched ? collisionMatches.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        if (count <= 4 || (matched && accepted <= 4)) {
            const auto savedMath = _mm_getcsr();
            std::fprintf(stderr, "[NoclipProbe] hook=collision descriptor=%08X originalActive=%u selectedActor=%08X selectedState=%08X matched=%u\n",
                         descriptor.u32, active.u32, actor, state, unsigned(matched));
            _mm_setcsr(savedMath);
        }
    }
    if (matched) active.u64 = 0;
}
