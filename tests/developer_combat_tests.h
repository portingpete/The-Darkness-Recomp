#pragma once
#include "runtime/native/developer_combat.h"
#include "runtime/native/developer_player.h"

static void testDeveloperCombat(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    auto* base = memory->base();
    const auto block = memory->allocate(0x5000);
    check(block != 0, "Combat cheat fixture allocation");
    struct Restore {
        uint32_t block;
        ~Restore() { configureDeveloperCombat(nullptr, {}, {}); memory->release(block); }
    } restore{block};

    const auto server = block, actor = block + 0x1000, state = block + 0x1800;
    const auto weapon = block + 0x2000, observer = block + 0x3000;
    constexpr uint32_t playerId = 42;
    const DeveloperPlayerHandles player{server, actor, state};
    PPC_STORE_U16(actor + 368, playerId);
    PPC_STORE_U32(actor + 396, state);
    PPC_STORE_U32(actor + 636, server);
    PPC_STORE_U32(weapon + 16, server);
    PPC_STORE_U32(observer + 32, server);
    PPC_STORE_U32(observer + 28, actor + 0x400);
    // Make the original predicate take its existing no-target return with the
    // override enabled, and its observer mode 4 success path when disabled.
    PPC_STORE_U32(actor + 0x400 + 420, 4 << 12);
    check(memory->read32(0x82325D1C) == 0xB17F018C && memory->read32(0x82507754) == 0x2B040000,
          "Combat cheats require the retail shot store and target predicate");

    auto fire = [&](uint32_t shooter) {
        auto call = ctx;
        call.r3.u64 = weapon;
        call.r4.u64 = 1;
        call.r7.u64 = shooter;
        call.r8.u64 = 0;
        sub_82325CA8(call, base);
        return PPC_LOAD_U16(weapon + 396);
    };
    auto eligible = [&](uint32_t candidate) {
        auto call = ctx;
        call.r3.u64 = observer;
        call.r4.u64 = candidate;
        sub_82507748(call, base);
        return call.r3.u32;
    };

    PPC_STORE_U16(weapon + 396, 6);
    PPC_STORE_U16(weapon + 398, 8);
    PPC_STORE_U16(weapon + 400, 0);
    PPC_STORE_U32(weapon + 584, 0);
    check(fire(playerId) == 5 && eligible(actor) == 1, "Default combat uses retail behavior");
    configureDeveloperCombat(base, player, {true, false});
    check(fire(playerId) == 5 && fire(playerId + 1) == 4 && eligible(actor) == 1,
          "Infinite ammo must preserve only the local player's loaded rounds");
    PPC_STORE_U32(weapon + 16, server + 0x400);
    check(fire(playerId) == 3, "A matching actor ID on a different server must consume ammo");
    PPC_STORE_U32(weapon + 16, server);
    check(PPC_LOAD_U16(weapon + 400) == 4, "The original shot sequence must still advance");

    configureDeveloperCombat(base, player, {true, true});
    check(eligible(actor) == 0 && eligible(actor + 0x500) == 1,
          "The target override must reject only the selected player");
    PPC_STORE_U32(observer + 32, server + 0x400);
    check(eligible(actor) == 1, "Observers from another server must retain normal targeting");
    PPC_STORE_U32(observer + 32, server);
    PPC_STORE_U32(actor + 396, state + 0x100);
    check(fire(playerId) == 2 && eligible(actor) == 1, "Both cheats must reject a stale player state");
    PPC_STORE_U32(actor + 396, state);
    resetDeveloperPlayer();
    check(fire(playerId) == 1 && eligible(actor) == 1, "Player reset must restore both original combat paths");
    puts("Developer combat: original shot and target routines, player scope and reversal passed.");
}
