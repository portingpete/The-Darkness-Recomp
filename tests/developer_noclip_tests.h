#pragma once
#include "runtime/native/developer_noclip.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace DeveloperNoclipFixture {
inline thread_local uint32_t animation = 0;
inline thread_local unsigned collisions = 0;
inline thread_local uint32_t sweepingActor = 0, candidateMatrix = 0;
inline thread_local unsigned commits = 0;
struct CollisionBoundary {};
struct CommitBoundary {};
static PPC_FUNC(animationData) { ctx.r3.u64 = animation; }
static PPC_FUNC(absentObject) { ctx.r3.u64 = 0; }
static PPC_FUNC(collision) {
    ++collisions;
    // Stop at the original world-service boundary. The original movement,
    // facing, noclip predicates and gravity code run against fixture memory.
    throw CollisionBoundary{};
}
static PPC_FUNC(physicsResourceBoundary) {
    // The original sweep requests model bounds through821046F8 after its
    // descriptor gate. The same resource is requested by the ordinary bounds
    // refresh after a successful commit; distinguish those actual phases by
    // the matrix which the original server has already copied to the actor.
    if (std::memcmp(base + sweepingActor + 16, base + candidateMatrix, 64) == 0) {
        ++commits;
        throw CommitBoundary{};
    }
    ++collisions;
    throw CollisionBoundary{};
}
static PPC_FUNC(commitRefreshBoundary) {
    ++commits;
    throw CommitBoundary{};
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

static void testDeveloperNoclip(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    using namespace DeveloperNoclipFixture;
    auto* base = memory->base();
    const auto block = memory->allocate(0x50000);
    check(block != 0, "Noclip fixture allocation");
    struct Restore {
        uint32_t block;
        ~Restore() { configureDeveloperNoclip(nullptr, 0, 0, false, false); animation = sweepingActor = candidateMatrix = 0; memory->release(block); }
    } restore{block};
    std::memset(base + block, 0, 0x50000);
    const auto actor = block, state = block + 0x1000, server = block + 0x5000;
    const auto table = block + 0x6000, animationTable = block + 0x6800;
    const auto output = block + 0x7000, npc = block + 0x9000, npcState = block + 0xA000;
    animation = block + 0x6900;
    std::memcpy(base + table, base + 0x82080F80, 0x800);
    memory->write32(server, table);
    memory->write32(table + 56, 0x820CA090);
    memory->write32(table + 84, 0x820CA088);
    memory->write32(table + 104, 0x820CA088);
    memory->write32(table + 112, 0x820CA088);
    memory->write32(animationTable + 48, 0x820CA098);
    Dispatch getAnimation(base, 0x820CA098, animationData);
    Dispatch worldCollision(base, 0x820CA090, collision);
    Dispatch noObject(base, 0x820CA088, absentObject);
    auto putFloat = [&](uint32_t address, float value) { memory->write32(address, std::bit_cast<uint32_t>(value)); };
    auto getFloat = [&](uint32_t address) { return std::bit_cast<float>(memory->read32(address)); };
    putFloat(server + 412, 1.0f / 30);
    for (const auto [a, s] : std::array{std::pair{actor, state}, std::pair{npc, npcState}}) {
        memory->write32(a + 396, s);
        memory->write32(a + 636, server);
        memory->write32(s + 16, a);
        memory->write32(s + 56, server);
        memory->write32(s + 64, animationTable);
        memory->write32(s + 104, block + 0x6A00);
        memory->write32(s + 888, 1);
        memory->write32(s + 10240, 0x40000001);
        PPC_STORE_U16(s + 10386, 0xFFFF);
        putFloat(s + 10020, 2); putFloat(s + 10024, 8);
        putFloat(s + 10028, 6); putFloat(s + 10032, 3);
        putFloat(s + 10432, 1);
        putFloat(s + 7084, 0); putFloat(s + 7088, 0); putFloat(s + 7092, 1);
        putFloat(s + 7096, 0); putFloat(s + 7100, 0); putFloat(s + 7104, 1);
        putFloat(s + 10608, 1); putFloat(s + 10628, 1);
        PPC_STORE_U16(s + 10820, 0x40);
        // Match the live CharPlayer's normal selector and valid animation
        // metadata. Metadata+18 is zero, so its root-motion shortcut is inactive.
        PPC_STORE_U8(s + 10822, 0);
    }
    // Require the actual retail instruction boundaries used by the four movement hooks.
    for (const auto [address, opcode] : std::array{
            std::pair{0x82133E54u, 0x2F0A0000u}, std::pair{0x82137824u, 0x2F0A0000u},
            std::pair{0x82175F9Cu, 0x396BFFFCu}, std::pair{0x821720E8u, 0x2F0B0004u}})
        check(memory->read32(address) == opcode, "Noclip hook must match the original movement-mode predicate");
    check(memory->read32(0x82134218) == 0x2F100004 && memory->read32(0x8213421C) == 0x419A1A60,
          "Original generic mode4 must branch to the original free-movement path");
    auto evaluate = [&](uint32_t a, uint32_t s, uint64_t original, bool stateOnly = false) {
        PPCRegister owner{}, component{}, mode{};
        owner.u64 = a; component.u64 = s; mode.u64 = original;
        if (stateOnly) ApplyDeveloperNoclipStateMidAsmHook(component, mode);
        else ApplyDeveloperNoclipMidAsmHook(owner, component, mode);
        return mode.u64;
    };
    configureDeveloperNoclip(base, actor, state, true, false);
    check(evaluate(actor, state, 2) == 2 && evaluate(actor, state, 2, true) == 2,
          "Unused developer tools must preserve original movement modes");
    configureDeveloperNoclip(base, actor, state, true, true);
    check(evaluate(actor, state, 2) == 4 && evaluate(actor, state, 2, true) == 4 &&
          evaluate(npc, npcState, 2) == 2 && evaluate(npc, npcState, 2, true) == 2,
          "Noclip must scope both original player paths to the selected actor/state");
    memory->write32(actor + 396, npcState);
    check(evaluate(actor, state, 2) == 2 && evaluate(actor, state, 2, true) == 2,
          "Replaced player state must reject stale noclip ownership");
    memory->write32(actor + 396, state);
    memory->write32(state + 16, npc);
    check(evaluate(actor, state, 2, true) == 2, "State-only collision hook must verify the original owner link");
    memory->write32(state + 16, actor);
    memory->write32(actor + 364, 0x00100000);
    check(evaluate(actor, state, 2) == 2 && evaluate(actor, state, 2, true) == 2,
          "Noclip must honor the original command's separate flying-mode exclusion");
    memory->write32(actor + 364, 0);

    struct Motion { bool collided; std::array<float, 3> displacement; };
    auto move = [&](bool derived, uint32_t a, uint32_t s, uint32_t authoredMode,
                    float forward, float side, float up) {
        memory->write32(a + 420, authoredMode << 12);
        putFloat(s + 1016, forward); putFloat(s + 1020, side); putFloat(s + 1024, up);
        std::memset(base + output, 0, 64);
        collisions = 0;
        auto call = ctx; call.r1.u64 = block + 0x4F000;
        if (derived) {
            call.r3.u64 = s; call.r4.u64 = output;
        } else {
            call.r3.u64 = a; call.r4.u64 = 0; call.r5.u64 = server;
            call.r6.u64 = 0; call.r9.u64 = output; call.r10.u64 = 0;
        }
        try {
            if (derived) sub_82175F58(call, base);
            else sub_82136B10(call, base);
            check(collisions == 0, "Original noclip must return before world collision queries");
            return Motion{false, {getFloat(output + 48), getFloat(output + 52), getFloat(output + 56)}};
        } catch (const CollisionBoundary&) {
            check(collisions == 1, "Original ordinary movement must reach the world collision service");
            return Motion{true, {}};
        }
    };
    auto sameMotion = [&](const Motion& expected, const Motion& actual) {
        check(!expected.collided && !actual.collided, "Noclip must run the original collision-free branch");
        for (unsigned i = 0; i < 3; ++i)
            check(std::isfinite(actual.displacement[i]) &&
                  std::abs(expected.displacement[i] - actual.displacement[i]) < 0.00001f,
                  "Developer noclip must match original mode4 displacement including omitted gravity");
    };
    for (bool derived : {false, true}) {
        for (const auto input : std::array{std::array{0.5f, 0.25f, 0.0f},
                                           std::array{0.0f, 0.0f, 0.0f},
                                           std::array{-0.5f, -0.25f, 0.0f}}) {
            configureDeveloperNoclip(base, actor, state, false, true);
            const auto original = move(derived, actor, state, 4, input[0], input[1], input[2]);
            configureDeveloperNoclip(base, actor, state, true, true);
            sameMotion(original, move(derived, actor, state, 0, input[0], input[1], input[2]));
            check(memory->read32(actor + 420) == 0 && memory->read32(actor + 364) == 0,
                  "Noclip must not save its mode or alter replicated actor flags");
        }
        const auto movement = move(derived, actor, state, 0, 0.5f, 0.25f, 0);
        check(std::abs(movement.displacement[0]) + std::abs(movement.displacement[1]) > 0.001f,
              "Original noclip fixture must actually move the player");
        check(move(derived, npc, npcState, 0, 0.5f, 0.25f, 0).collided,
              "Original NPC movement must retain collision while player noclip is enabled");
        configureDeveloperNoclip(base, actor, state, false, true);
        check(move(derived, actor, state, 0, 0.5f, 0.25f, 0).collided,
              "Turning noclip off must restore original world collision immediately");
        const auto noInput = move(derived, actor, state, 4, 0, 0, 0);
        check(!noInput.collided && std::abs(noInput.displacement[2]) < 0.00001f,
              "Original noclip and its developer override must omit gravity");
    }
    check(memory->read32(state + 10240) == 0x40000001 && memory->read32(state + 10024) == std::bit_cast<uint32_t>(8.0f) &&
          memory->read32(state + 10028) == std::bit_cast<uint32_t>(6.0f) &&
          memory->read32(state + 10032) == std::bit_cast<uint32_t>(3.0f),
          "Noclip must preserve saved player flags and authored movement speeds");
    configureDeveloperNoclip(base, actor, state, true, true);
    configureDeveloperNoclip(nullptr, 0, 0, false, false);
    check(evaluate(actor, state, 2) == 2 && evaluate(actor, state, 2, true) == 2,
          "Unavailable player or developer reset must clear noclip before another update");

    // The original server independently sweeps the returned displacement:
    //82474FF0 delegates through service56 to82472DF0, then copies the candidate
    // matrix only when that query reports no collision. Run both functions;
    // stop at a resource/bounds service after the original descriptor gate.
    check(memory->read32(0x82472E18) == 0x89780038 && memory->read32(0x82472E1C) == 0x2B0B0000 &&
          memory->read32(0x82472E20) == 0x409A0018,
          "Outer noclip hook must match the original descriptor byte56 predicate");
    const auto resources = block + 0x18000, resourceVector = block + 0x18100;
    const auto resourceArray = block + 0x18200, resource = block + 0x18300, resourceTable = block + 0x18400;
    const auto objectVector = block + 0x18500, objectArray = block + 0x18600;
    const auto oldMatrix = block + 0x1C000;
    candidateMatrix = block + 0x1C100;
    memory->write32(server + 432, resources);
    memory->write32(resources + 12, resourceVector);
    memory->write32(resourceVector + 4, 1);
    memory->write32(resourceVector + 24, resourceArray);
    memory->write32(resourceArray, resource);
    memory->write32(resource, resourceTable);
    memory->write32(resourceTable + 36, 0x820CA0B0);
    memory->write32(server + 1460, objectVector);
    memory->write32(objectVector + 24, objectArray);
    memory->write32(objectArray + 4, actor);
    memory->write32(objectArray + 8, npc);
    memory->write32(table + 56, 0x82472DF0);
    memory->write32(table + 108, 0x820CA0B8);
    Dispatch resourceBounds(base, 0x820CA0B0, physicsResourceBoundary);
    Dispatch refreshBounds(base, 0x820CA0B8, commitRefreshBoundary);
    for (unsigned i = 0; i < 4; ++i) {
        putFloat(oldMatrix + (i * 4 + i) * 4, 1);
        putFloat(candidateMatrix + (i * 4 + i) * 4, 1);
    }
    putFloat(candidateMatrix + 48, 12); putFloat(candidateMatrix + 52, 21); putFloat(candidateMatrix + 56, 34);
    for (const auto a : {actor, npc}) {
        // One original type1 model shape, resource0: its bounds service is the
        // fixture boundary. The resident descriptor remains active throughout.
        PPC_STORE_U16(a + 264 + 8, 0x2000);
        PPC_STORE_U8(a + 320, 1);
    }
    auto collisionGate = [&](uint32_t descriptor) {
        PPCRegister shape{}, active{}; shape.u64 = descriptor; active.u64 = 1;
        ApplyDeveloperNoclipCollisionMidAsmHook(shape, active);
        check(shape.u32 == descriptor, "Noclip must preserve the physics descriptor register");
        return active.u64;
    };
    configureDeveloperNoclip(base, actor, state, true, true);
    check(collisionGate(actor + 264) == 0 && collisionGate(npc + 264) == 1 && collisionGate(actor + 265) == 1,
          "Outer noclip collision gate must match only the selected player's exact descriptor");
    memory->write32(actor + 396, npcState);
    check(collisionGate(actor + 264) == 1, "Outer noclip must reject a replaced player state");
    memory->write32(actor + 396, state);
    memory->write32(state + 16, npc);
    check(collisionGate(actor + 264) == 1, "Outer noclip must reject a stale original owner link");
    memory->write32(state + 16, actor);
    memory->write32(actor + 364, 0x00100000);
    check(collisionGate(actor + 264) == 1, "Outer noclip must retain the original flying-mode exclusion");
    memory->write32(actor + 364, 0);
    auto sweep = [&](uint32_t a, bool authoredNoCollision) {
        std::memcpy(base + a + 16, base + oldMatrix, 64);
        std::memcpy(base + a + 80, base + oldMatrix, 64);
        PPC_STORE_U8(a + 320, authoredNoCollision ? 0 : 1);
        sweepingActor = a;
        collisions = commits = 0;
        auto call = ctx; call.r1.u64 = block + 0x4F000;
        call.r3.u64 = server; call.r4.u64 = 0; call.r5.u64 = a == actor ? 1 : 2;
        call.r6.u64 = oldMatrix; call.r7.u64 = candidateMatrix; call.r8.u64 = 0;
        try {
            sub_82474FF0(call, base);
            check(false, "Outer noclip fixture must reach an original resource/bounds service boundary");
        } catch (const CollisionBoundary&) {
            check(collisions == 1 && commits == 0 && std::memcmp(base + a + 16, base + oldMatrix, 64) == 0,
                  "Original collision bounds must be queried before the server commits player movement");
            return false;
        } catch (const CommitBoundary&) {
            check(commits == 1 && collisions == 0 && std::memcmp(base + a + 16, base + candidateMatrix, 64) == 0,
                  "Outer noclip must commit the complete original candidate matrix before bounds refresh");
            return true;
        }
        return false;
    };
    configureDeveloperNoclip(base, actor, state, false, true);
    check(sweep(actor, true), "Original inactive physics descriptor must accept the outer movement commit");
    check(!sweep(actor, false), "Original active physics descriptor must query collision before committing movement");
    configureDeveloperNoclip(base, actor, state, true, true);
    check(sweep(actor, false) && PPC_LOAD_U8(actor + 320) == 1,
          "Developer noclip must match original collision-free commit without changing the resident descriptor");
    check(!sweep(npc, false), "Unrelated NPC outer movement must retain its original collision query");
    configureDeveloperNoclip(base, actor, state, false, true);
    check(!sweep(actor, false), "Disabling noclip must immediately restore the outer server collision query");
    configureDeveloperNoclip(base, actor, state, true, true);
    configureDeveloperNoclip(nullptr, 0, 0, false, false);
    check(collisionGate(actor + 264) == 1, "Developer reset must clear the outer descriptor override");
    puts("Developer noclip: original generic/derived movement, gravity and outer server commit; NPC isolation, restoration and unchanged saved physics passed.");
}
