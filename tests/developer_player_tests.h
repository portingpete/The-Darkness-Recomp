#pragma once
#include "runtime/native/developer_player.h"
#include "runtime/native/developer_tools.h"
#include "developer_player_lookup_tests.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace DeveloperPlayerMovementFixture {
inline thread_local uint32_t componentData = 0;
inline thread_local bool walking = false;
inline thread_local std::array<float, 3> movement{};
struct Captured {};
static PPC_FUNC(animationData) { ctx.r3.u64 = componentData; }
static PPC_FUNC(noActor) { ctx.r3.u64 = 0; }
static PPC_FUNC(captureCollision) {
    // The original control handler has built its movement vector here. World
    // collision requires a live scene, so stop at that service boundary; none
    // of the authored speeds, input normalization or basis math are replaced.
    movement = walking ? std::array{float(ctx.f27.f64), float(ctx.f26.f64), float(ctx.f22.f64)}
                       : std::array{float(ctx.f26.f64), float(ctx.f23.f64), float(ctx.f27.f64)};
    throw Captured{};
}
struct Dispatch {
    uint8_t* base;
    uint32_t address;
    PPCFunc* previous;
    Dispatch(uint8_t* b, uint32_t a, PPCFunc* callback)
        : base(b), address(a), previous(PPC_LOOKUP_FUNC(b, a)) { PPC_LOOKUP_FUNC(base, address) = callback; }
    ~Dispatch() { PPC_LOOKUP_FUNC(base, address) = previous; }
};
}

static void testDeveloperPlayer(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    using namespace DeveloperPlayerMovementFixture;
    const auto unavailable = updateDeveloperPlayer(ctx, nullptr, 0, 1, false, false);
    check(!unavailable.hasActivePlayer && !unavailable.applied,
          "Missing player cannot report developer controls applied");
    DeveloperPlayerLookupFixture::Fixture lookup;
    auto queryContext = ctx;
    lookup.prepare(queryContext);
    auto* base = memory->base();
    const auto block = memory->allocate(0x40000);
    check(block != 0, "Developer movement fixture allocation");
    struct Restore {
        PPCContext& ctx;
        uint32_t block;
        ~Restore() {
            updateDeveloperPlayer(ctx, nullptr, 0, 1, false, false);
            componentData = 0;
            memory->release(block);
        }
    } restore{ctx, block};
    std::memset(base + block, 0, 0x40000);
    const auto state = lookup.state, actor = lookup.actor, server = lookup.server;
    const auto table = block, animTable = block + 0x800, animData = block + 0xA00;
    const auto output = block + 0x1000, input = output + 0x20, angles = output + 0x40;
    const auto flags = output + 0x60, resultFlags = output + 0x70, hashName = output + 0x80;
    std::memcpy(base + table, base + 0x82080F80, 0x800);
    memory->write32(table + 56, 0x820CA090);
    memory->write32(table + 104, 0x820CA088);
    memory->write32(animTable + 48, 0x820CA098);
    Dispatch getAnimation(base, 0x820CA098, animationData);
    Dispatch collision(base, 0x820CA090, captureCollision);
    Dispatch absentActor(base, 0x820CA088, noActor);
    componentData = animData;
    memory->write32(state + 64, animTable);
    PPC_STORE_U16(state + 10386, 0xFFFF);
    memory->write32(actor + 364, 0);
    auto putFloat = [&](uint32_t address, float value) { memory->write32(address, std::bit_cast<uint32_t>(value)); };
    auto getFloat = [&](uint32_t address) { return std::bit_cast<float>(memory->read32(address)); };
    putFloat(state + 10020, 2); // Walking endpoint.
    putFloat(state + 10024, 8); // Original SPEED_FORWARD.
    putFloat(state + 10028, 6); // Original SPEED_SIDESTEP.
    putFloat(state + 10032, 3); // Original SPEED_UP.
    putFloat(state + 10036, 11); // Original SPEED_JUMP.
    putFloat(server + 412, 1.0f / 30);
    const std::array authored{memory->read32(state + 10020), memory->read32(state + 10024),
        memory->read32(state + 10028), memory->read32(state + 10032), memory->read32(state + 10036)};
    const auto elapsed = memory->read32(server + 412);
    for (const auto [address, opcode] : std::array{
            std::pair{0x82134474u, 0xC1410218u}, std::pair{0x82135018u, 0xEC000632u},
            std::pair{0x82135020u, 0xED0C6828u}, std::pair{0x82135B08u, 0xEDAD0632u},
            std::pair{0x82135C8Cu, 0xEDAD0632u}}) {
        check(memory->read32(address) == opcode, "Movement hook must match original horizontal speed consumption");
    }
    // Independent retail name hashing ties the consumed fields to the authored
    // movement keys; these constants also select the original setter branches.
    for (const auto [name, expected] : std::array{
            std::pair{"SPEED_FORWARD", 0x98E0F105u}, std::pair{"SPEED_SIDESTEP", 0xC4980B31u},
            std::pair{"SPEED_UP", 0x3DB59675u}, std::pair{"SPEED_JUMP", 0x81C8788Cu}}) {
        std::strcpy(reinterpret_cast<char*>(base + hashName), name);
        auto call = ctx; call.r3.u64 = hashName; call.r4.u64 = 0;
        sub_821F5290(call, base);
        check(call.r3.u32 == expected, "Original movement property hash proof");
    }
    auto activate = [&](float multiplier, bool enabled = true) {
        memory->write32(server, 0x82080F80);
        const auto result = updateDeveloperPlayer(queryContext, base, lookup.client, multiplier, false, enabled);
        check(result.hasActivePlayer && result.applied == enabled, "Player update must resolve original server player");
        memory->write32(server, table);
    };
    auto move = [&](unsigned posture, bool freeMovement, float forward, float side, float up = 0,
                    uint32_t movedActor = 0) {
        const auto controlledActor = movedActor ? movedActor : actor;
        walking = (posture & 15) == 0;
        memory->write32(controlledActor + 420, freeMovement ? 0x4000 : posture << 8);
        putFloat(input, forward); putFloat(input + 4, side); putFloat(input + 8, up);
        memory->write32(flags, 0); PPC_STORE_U16(resultFlags, 0);
        auto call = ctx; call.r1.u64 = block + 0x3F000;
        call.r3.u64 = output; call.r4.u64 = 1; call.r5.u64 = controlledActor; call.r6.u64 = 0;
        call.r7.u64 = server; call.r8.u64 = input; call.r9.u64 = angles; call.r10.u64 = 0;
        memory->write32(call.r1.u32 + 84, flags);
        memory->write32(call.r1.u32 + 92, 0);
        memory->write32(call.r1.u32 + 100, resultFlags);
        try {
            sub_82133DD8(call, base);
            check(freeMovement, "Ground movement fixture must reach the original world collision boundary");
            return std::array{getFloat(output), getFloat(output + 4), getFloat(output + 8)};
        } catch (const Captured&) {
            check(!freeMovement, "Free-movement original handler must return its displacement");
            return movement;
        }
    };
    auto closeEnough = [](float a, float b) { return std::isfinite(a) && std::abs(a - b) < 0.0001f; };
    auto twice = [&](const auto& before, const auto& after) {
        check(std::abs(before[0]) + std::abs(before[1]) > 0.001f,
              "Original movement fixture must produce a horizontal vector");
        check(closeEnough(after[0], before[0] * 2) && closeEnough(after[1], before[1] * 2),
              "Developer multiplier must double original movement without changing input or elapsed time");
    };
    // Selector0 is the normal ground path; selector5 uses its other authored
    // horizontal branch. High mode2 (0x2000) exercises the original half-input
    // crouch gate before the ground/walking interpolation.
    for (const unsigned posture : {0u, 5u, 0x20u}) {
        activate(1); const auto baseline = move(posture, false, 0.5f, 0.25f);
        activate(2); twice(baseline, move(posture, false, 0.5f, 0.25f));
        twice(baseline, move(posture, false, 0.5f, 0.25f)); // Never compounds per update/call.
        activate(1); const auto restored = move(posture, false, 0.5f, 0.25f);
        check(closeEnough(restored[0], baseline[0]) && closeEnough(restored[1], baseline[1]), "Defaults restore original ground/crouch movement");
    }
    activate(1); const auto baseline = move(0, true, 1, 0.5f, 1);
    activate(2); const auto accelerated = move(0, true, 1, 0.5f, 1);
    twice(baseline, accelerated);
    check(closeEnough(baseline[2], accelerated[2]), "Horizontal player speed must leave original vertical movement unchanged");
    memory->write32(lookup.cameraState + 64, animTable);
    PPC_STORE_U16(lookup.cameraState + 10386, 0xFFFF);
    memory->write32(lookup.cameraActor + 364, 0);
    for (unsigned i = 0; i < authored.size(); ++i)
        memory->write32(lookup.cameraState + 10020 + i * 4, authored[i]);
    const auto npcMovement = move(0, true, 1, 0.5f, 1, lookup.cameraActor);
    check(closeEnough(npcMovement[0], baseline[0]) && closeEnough(npcMovement[1], baseline[1]) && closeEnough(npcMovement[2], baseline[2]),
          "Actual original NPC movement must remain unchanged while player speed is enabled");
    resetDeveloperTools();
    const auto resetMovement = move(0, true, 1, 0.5f, 1);
    check(closeEnough(resetMovement[0], baseline[0]) && closeEnough(resetMovement[1], baseline[1]),
          "Reset must immediately clear applied player speed before the next developer update");
    activate(2);
    const auto ownedState = memory->read32(actor + 396);
    memory->write32(actor + 396, state + 0x3000);
    // The original field loads still address the fixture state, but the selected
    // actor's state identity has changed: stale ownership must be ignored.
    PPCRegister a{}, s{}, forward{}, side{};
    a.u64 = actor; s.u64 = state; forward.f64 = 8; side.f64 = 6;
    ApplyDeveloperHorizontalSpeedMidAsmHook(a, s, forward, side);
    check(forward.f64 == 8 && side.f64 == 6, "Changed actor state must reject stale developer speed ownership");
    memory->write32(actor + 396, ownedState);
    a.u64 = actor + 0x100; forward.f64 = 8; side.f64 = 6;
    ApplyDeveloperHorizontalSpeedMidAsmHook(a, s, forward, side);
    check(forward.f64 == 8 && side.f64 == 6, "Developer player speed cannot affect NPC movement");
    activate(2, false); const auto unused = move(0, true, 1, 0.5f, 1);
    check(closeEnough(unused[0], baseline[0]) && closeEnough(unused[1], baseline[1]), "Unused controls must preserve original player movement");
    for (unsigned i = 0; i < authored.size(); ++i)
        check(memory->read32(state + 10020 + i * 4) == authored[i], "Developer speed must leave saved/authored movement fields unchanged");
    check(memory->read32(server + 412) == elapsed, "Developer speed must leave world timing unchanged");
    memory->write32(server, 0x82080F80);
    puts("Developer player: original ground/crouch/free movement handler, vertical and NPC isolation, restoration and unchanged saved state passed.");
}
