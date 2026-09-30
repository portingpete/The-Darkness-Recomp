#pragma once
#include "runtime/native/developer_invincibility.h"
#include <array>

static void testDeveloperInvincibility(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    auto* base = memory->base();
    const auto block = memory->allocate(0x8000);
    check(block != 0, "Invincibility fixture allocation");
    struct Restore {
        uint32_t block;
        ~Restore() { configureDeveloperInvincibility(nullptr, 0, 0, false, false); memory->release(block); }
    } restore{block};
    const auto actor = block, state = block + 0x1000;
    memory->write32(actor + 396, state);
    memory->write32(state + 10240, 0x40000001);
    // Each original hook boundary is exactly the god-bit isolation following
    // a state flags load. It executes unchanged after the native register hook.
    for (const auto address : std::array{0x8214EC50u, 0x82154660u, 0x82162204u, 0x82189B08u}) {
        check(memory->read32(address) == 0x556B05AC,
              "Invincibility hook requires original rlwinm r11,r11,0,22,22 god-mode predicate");
    }
    PPCRegister player{}, component{}, flags{};
    player.u64 = actor; component.u64 = state;
    auto evaluate = [&](uint32_t a, uint32_t s, uint32_t original) {
        player.u64 = a; component.u64 = s; flags.u64 = original;
        ApplyDeveloperInvincibilityMidAsmHook(player, component, flags);
        return flags.u32;
    };
    configureDeveloperInvincibility(base, actor, state, true, false);
    check(evaluate(actor, state, 1) == 1, "Unused developer tools cannot change original god-mode behavior");
    configureDeveloperInvincibility(base, actor, state, true, true);
    check(evaluate(actor, state, 1) == 0x201 && evaluate(actor + 512, state, 1) == 1 &&
          evaluate(actor, state + 512, 1) == 1, "Invincibility must affect only the selected server player/state");
    memory->write32(actor + 396, state + 512);
    check(evaluate(actor, state, 1) == 1,
          "Changed actor state must reject stale developer invincibility ownership");
    memory->write32(actor + 396, state);
    check(evaluate(actor, state, 1) == 0x201, "Invincibility must remain armed for the current actor state");
    check(memory->read32(state + 10240) == 0x40000001,
          "Invincibility must never alter checkpoint/replication state flags");
    configureDeveloperInvincibility(base, actor, state, false, true);
    check(evaluate(actor, state, 1) == 1 && evaluate(actor, state, 0x201) == 0x201,
          "Turning invincibility off must restore original predicates without clearing authored flags");
    puts("Developer invincibility: original god predicates, player-only scope, disable and unchanged saved flags passed.");
}
