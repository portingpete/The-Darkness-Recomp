#pragma once
#include "runtime/native/developer_player_lookup.h"
#include <array>
#include <cstring>

namespace DeveloperPlayerLookupFixture {
constexpr uint32_t queryAddress = 0x820C1004, typeAddress = 0x820C1014;
inline uint32_t queriedSystem, queriedContext, returnedDescriptor;
inline unsigned queries;
inline bool returnContext = true;

static PPC_FUNC(query) {
    check(ctx.r4.u32 == queriedSystem && ctx.r5.u32 == 0x820515EC &&
          std::strcmp(reinterpret_cast<char*>(base + ctx.r5.u32), "GAMECONTEXT") == 0,
          "player lookup queried the wrong original service/ABI");
    ++queries;
    if (returnContext) {
        memory->write32(queriedContext + 4, memory->read32(queriedContext + 4) + 1);
        memory->write32(ctx.r3.u32, queriedContext);
    } else memory->write32(ctx.r3.u32, 0);
    ctx.r3.u64 = 0xBAD0BAD0;
    ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(type) {
    check(ctx.r3.u32 == queriedContext, "player lookup inspected an unrelated service class");
    ctx.r3.u64 = returnedDescriptor;
}

// Shared with actual movement tests: only GAMECONTEXT service lookup/type are
// substituted. Server actor lookup82448928 and reference release820C1100 run
// as original AOT code against committed fixture memory.
struct Fixture {
    uint8_t* base = memory->base();
    uint32_t block = memory->allocate(0x40000);
    uint32_t client = block, gameContext = block + 0x4000, server = block + 0x8000;
    uint32_t actor = block + 0xB000, state = block + 0xC000;
    uint32_t contextTable = block + 0xF000, descriptor = block + 0xF040;
    uint32_t systemTable = block + 0xF080, objects = block + 0xF200, items = block + 0xF240;
    uint32_t system = block + 0xF300, cameraActor = block + 0x11000, cameraState = block + 0x12000;
    uint32_t stack = block + 0x3F000;
    uint32_t oldSystem = memory->read32(0x82A690F8), oldToken = memory->read32(0x82A40308);
    PPCFunc* oldQuery = PPC_LOOKUP_FUNC(base, queryAddress);
    PPCFunc* oldType = PPC_LOOKUP_FUNC(base, typeAddress);

    Fixture() {
        check(block != 0, "server player lookup fixture allocation failed");
        std::memset(base + block, 0, 0x40000);
        queriedSystem = system; queriedContext = gameContext; returnedDescriptor = descriptor;
        queries = 0; returnContext = true;
        memory->write32(client, 0x82081BF0);
        memory->write32(client + 536, 3);
        memory->write32(client + 540, 4); // Distinct camera NPC must never be selected.
        memory->write32(gameContext, contextTable); memory->write32(gameContext + 4, 1);
        memory->write32(gameContext + 3660, server);
        memory->write32(contextTable, typeAddress);
        // A derived descriptor must resolve through its base chain.
        memory->write32(descriptor, 0xD3110000); memory->write32(descriptor + 8, descriptor + 16);
        memory->write32(descriptor + 16, 0xD311CACE);
        memory->write32(system, systemTable); memory->write32(systemTable + 36, queryAddress);
        memory->write32(server, 0x82080F80); memory->write32(server + 1460, objects);
        memory->write32(objects + 4, 8); memory->write32(objects + 24, items);
        memory->write32(items + 3 * 4, actor); memory->write32(items + 4 * 4, cameraActor);
        PPC_STORE_U16(actor + 368, 3); PPC_STORE_U16(actor + 370, 7);
        memory->write32(actor + 396, state); memory->write32(actor + 636, server);
        PPC_STORE_U16(cameraActor + 368, 4); PPC_STORE_U16(cameraActor + 370, 9);
        memory->write32(cameraActor + 396, cameraState); memory->write32(cameraActor + 636, server);
        memory->write32(0x82A690F8, system); memory->write32(0x82A40308, 0xD311CACE);
        PPC_LOOKUP_FUNC(base, queryAddress) = query;
        PPC_LOOKUP_FUNC(base, typeAddress) = type;
    }
    explicit Fixture(PPCContext& ctx) : Fixture() { prepare(ctx); }
    // An optional isolated guest stack for movement AOT calls. Preparing a
    // context does not reset player/component data populated by the caller.
    void prepare(PPCContext& ctx) const { ctx.r1.u64 = stack; }
    ~Fixture() {
        PPC_LOOKUP_FUNC(base, queryAddress) = oldQuery;
        PPC_LOOKUP_FUNC(base, typeAddress) = oldType;
        memory->write32(0x82A690F8, oldSystem);
        memory->write32(0x82A40308, oldToken);
        memory->release(block);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};
}

static void testDeveloperPlayerLookup(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    using namespace DeveloperPlayerLookupFixture;
    Fixture fixture;
    auto* base = fixture.base;
    const auto before = ctx;
    const auto resolved = resolveDeveloperPlayer(ctx, fixture.base, fixture.client);
    check(resolved.server == fixture.server && resolved.actor == fixture.actor && resolved.state == fixture.state,
          "original server lookup did not select the current local-player actor/state");
    check(resolved.actor != fixture.cameraActor && queries == 1,
          "camera actor or a guessed client-mirror pointer entered player tools");
    check(std::memcmp(&ctx, &before, sizeof(ctx)) == 0 && memory->read32(fixture.gameContext + 4) == 1,
          "player lookup changed caller context or leaked its GAMECONTEXT reference");
    // Match the live retail campaign client and a high actor-vector index.
    // Original class getters prove Mod/P6 identity from retail descriptors;
    // original82448928 still resolves the actor, without an array callback.
    const auto campaignItems = fixture.block + 0x20000;
    memory->write32(fixture.client, 0x820807E0);
    memory->write32(fixture.client + 516, 0x81);
    memory->write32(fixture.client + 536, 2559);
    PPC_STORE_U16(fixture.actor + 368, 2559);
    memory->write32(fixture.objects + 4, 2560);
    memory->write32(fixture.objects + 24, campaignItems);
    memory->write32(campaignItems + 2559 * 4, fixture.actor);
    for (const auto serverTable : std::array{0x82072370u, 0x82080F80u}) {
        memory->write32(fixture.server, serverTable);
        auto typeContext = ctx;
        typeContext.r3.u64 = fixture.server;
        PPCSafeIndirect(typeContext, base, memory->read32(serverTable));
        const auto name = memory->read32(typeContext.r3.u32);
        check(std::strcmp(reinterpret_cast<char*>(base + name),
              serverTable == 0x82072370 ? "CWServer_Mod" : "CWServer_P6") == 0,
              "accepted server table must match its original retail class descriptor");
        if (serverTable == 0x82080F80)
            check(memory->read32(typeContext.r3.u32 + 8) == 0x82A45948,
                  "original P6 descriptor must inherit CWServer_Mod");
        const auto campaign = resolveDeveloperPlayer(ctx, base, fixture.client);
        check(campaign.server == fixture.server && campaign.actor == fixture.actor && campaign.state == fixture.state,
              "retail campaign server must resolve its high-index local player through original lookup");
    }
    memory->write32(fixture.client, 0x82081BF0);
    memory->write32(fixture.client + 516, 0);
    memory->write32(fixture.client + 536, 3);
    PPC_STORE_U16(fixture.actor + 368, 3);
    memory->write32(fixture.objects + 4, 8);
    memory->write32(fixture.objects + 24, fixture.items);
    check(!developerMissionTransitionPending(ctx, fixture.base), "neutral preloading flag reported a transition");
    PPC_STORE_U8(fixture.gameContext + 4908, 1);
    check(developerMissionTransitionPending(ctx, fixture.base) &&
          !resolveDeveloperPlayer(ctx, fixture.base, fixture.client).actor,
          "preloading must suppress stale player handles");
    PPC_STORE_U8(fixture.gameContext + 4908, 0);

    const auto expectUnavailable = [&](const char* message) {
        const auto unavailable = resolveDeveloperPlayer(ctx, fixture.base, fixture.client);
        check(!unavailable.server && !unavailable.actor && !unavailable.state, message);
        check(memory->read32(fixture.gameContext + 4) == 1 && std::memcmp(&ctx, &before, sizeof(ctx)) == 0,
              "rejected player lookup leaked a reference or changed caller context");
    };
    memory->write32(fixture.client + 536, 0xFFFFFFFF);
    expectUnavailable("missing local player incorrectly fell back to the camera NPC");
    memory->write32(fixture.client + 536, 8);
    expectUnavailable("out-of-range player ID reached the server object vector");
    memory->write32(fixture.client + 536, 3);
    memory->write32(fixture.client + 516, 0x20);
    expectUnavailable("client transition accepted stale actor handles");
    memory->write32(fixture.client + 516, 0);
    memory->write32(fixture.server, 0x82081BF0);
    expectUnavailable("client world masqueraded as the actual server");
    memory->write32(fixture.server, 0x82080F80);
    memory->write32(fixture.actor + 636, fixture.client);
    expectUnavailable("actor from a different world entered player tools");
    memory->write32(fixture.actor + 636, fixture.server);
    memory->write32(fixture.actor + 396, 0);
    expectUnavailable("missing actor state entered damage/movement tools");
    memory->write32(fixture.actor + 396, fixture.state);
    PPC_STORE_U16(fixture.actor + 370, 0);
    expectUnavailable("uninitialized actor template ID entered player tools");
    PPC_STORE_U16(fixture.actor + 370, 7);
    memory->write32(fixture.objects + 24, 0xFFFFFFFC);
    expectUnavailable("overflowed server object vector address was accepted");
    memory->write32(fixture.objects + 24, fixture.items);
    memory->write32(fixture.descriptor + 8, fixture.descriptor);
    expectUnavailable("cyclic unrelated GAMECONTEXT class descriptor was accepted");
    memory->write32(fixture.descriptor + 8, fixture.descriptor + 16);
    returnContext = false;
    expectUnavailable("unavailable original GAMECONTEXT service supplied player handles");
    returnContext = true;
    memory->write32(0x82A690F8, 0);
    expectUnavailable("missing original system supplied player handles");
    memory->write32(0x82A690F8, fixture.system);
    auto noStack = ctx; noStack.r1.u64 = 0;
    check(!resolveDeveloperPlayer(noStack, fixture.base, fixture.client).actor &&
          !resolveDeveloperPlayer(ctx, nullptr, fixture.client).actor,
          "invalid stack or address space reached guest service calls");
    puts("Developer player lookup: original GAMECONTEXT/server ABI, local-player-only identity, context/refcount and transition/invalid-span guards passed.");
}
