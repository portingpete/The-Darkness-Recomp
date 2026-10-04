#pragma once
#include "runtime/native/developer_darkness.h"
#include "runtime/native/developer_tools.h"
#include "developer_player_lookup_tests.h"
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

static void testDeveloperDarkness(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    DeveloperPlayerLookupFixture::Fixture fixture;
    auto* base = fixture.base;
    const auto state = fixture.state;
    const auto player = resolveDeveloperPlayer(ctx, base, fixture.client);
    check(player.actor == fixture.actor && player.state == state, "Darkness fixture must resolve the original local player");
    const auto originalContext = ctx;
    const auto snapshot = [&] { return std::vector<uint8_t>(base + state, base + state + 10244); };
    const auto npcBefore = std::vector<uint8_t>(base + fixture.cameraState, base + fixture.cameraState + 10244);

    // The retail's named bit table ties the grant to its six concrete powers,
    // independently of the native implementation's numeric grant mask.
    const std::array<std::string_view, 6> names{
        "CreepingDark", "DemonArm", "AncientWeapons", "BlackHole", "DarknessVision", "DarknessShield"};
    for (unsigned i = 0; i < names.size(); ++i) {
        const auto name = memory->read32(0x82A473AC + i * 4);
        check(std::string_view(reinterpret_cast<char*>(base + name)) == names[i],
              "Darkness grant bits must match the original retail power-name table");
    }
    const auto getLevel = [&] {
        auto call = ctx;
        call.r3.u64 = state + 7668;
        sub_821A8E40(call, base);
        return call.r3.u32;
    };
    // Execute both original functions: this is the engine's threshold oracle,
    // rather than a second copy of the host grant implementation.
    const std::array<uint16_t, 5> thresholds{0, 15, 55, 110, 180};
    for (unsigned i = 0; i < thresholds.size(); ++i) {
        auto call = ctx;
        call.r3.u64 = state + 7668; call.r4.u64 = i;
        sub_821A8CF0(call, base);
        check(PPC_LOAD_U16(state + 7672) == thresholds[i] && getLevel() == i,
              "Original Darkness level setter/getter thresholds changed");
        if (i) {
            PPC_STORE_U16(state + 7672, thresholds[i] - 1);
            check(getLevel() == i - 1, "Original Darkness level getter must honor the exact heart boundary");
        }
    }
    auto clamped = ctx; clamped.r3.u64 = state + 7668; clamped.r4.u64 = 99;
    sub_821A8CF0(clamped, base);
    check(PPC_LOAD_U16(state + 7672) == 180 && getLevel() == 4, "Retail Darkness levels clamp at displayed level5");

    const auto prepare = [&] {
        std::memset(base + state, 0xA5, 10244);
        memory->write32(state + 7076, 0x80000201);
        PPC_STORE_U8(state + 7656, 17);
        PPC_STORE_U8(state + 7659, 0x40);
        PPC_STORE_U8(state + 7660, 0x80);
        PPC_STORE_U16(state + 7672, 14);
        PPC_STORE_U8(state + 8553, 20);
    };
    const auto onlyChanged = [&](const std::vector<uint8_t>& before, bool unlock, bool maximum) {
        for (unsigned i = 0; i < before.size(); ++i) {
            const bool expected = (i >= 7076 && i < 7080) ||
                (unlock && (i == 7659 || i == 7660)) ||
                (maximum && (i == 7656 || i == 8553 || i == 7672 || i == 7673));
            check(expected || before[i] == base[state + i], "Darkness grant changed an unrelated player/progression field");
        }
        check(std::memcmp(base + fixture.cameraState, npcBefore.data(), npcBefore.size()) == 0,
              "Darkness grant affected the camera NPC");
        check(std::memcmp(&ctx, &originalContext, sizeof(ctx)) == 0, "Darkness grant leaked guest registers into its caller");
    };
    prepare();
    auto before = snapshot();
    const auto neutral = applyDeveloperDarkness(ctx, base, player, false, false);
    check(!neutral.applied && neutral.status.empty() && before == snapshot(), "Unused Darkness tools must leave progression unchanged");

    const auto unlocked = applyDeveloperDarkness(ctx, base, player, true, false);
    check(unlocked.applied && PPC_LOAD_U8(state + 7660) == 0xBF && PPC_LOAD_U8(state + 7659) == 0x7F,
          "Unlock must grant all six usable/unlocked powers and retain each field's unrelated high bits");
    check(PPC_LOAD_U16(state + 7672) == 14 && PPC_LOAD_U8(state + 7656) == 17 && PPC_LOAD_U8(state + 8553) == 20,
          "Power unlock must not also grant a Darkness level or energy");
    check(memory->read32(state + 7076) == 0x80000301, "Power grant must preserve original dirty flags and add the power-update flag");
    onlyChanged(before, true, false);

    prepare(); before = snapshot();
    const auto maximum = applyDeveloperDarkness(ctx, base, player, false, true);
    check(maximum.applied && PPC_LOAD_U16(state + 7672) == 180 && getLevel() == 4,
          "Maximum Darkness grant must reach the original engine's displayed level5");
    check(PPC_LOAD_U8(state + 7656) == 100 && PPC_LOAD_U8(state + 8553) == 100,
          "Maximum Darkness grant must match original level5 capacity/current energy");
    check(PPC_LOAD_U8(state + 7659) == 0x40 && PPC_LOAD_U8(state + 7660) == 0x80,
          "Maximum Darkness level must not also unlock powers");
    check(memory->read32(state + 7076) == 0x80004301, "Level grant must add both original energy and capacity dirty flags");
    onlyChanged(before, false, true);

    // This original save helper serializes the granted heart count and the two
    // adjacent ability-upgrade flags. Prove the grant can survive that path,
    // without touching any filesystem/profile or triggering achievements.
    const auto cursor = fixture.block + 0x18000, output = cursor + 16;
    memory->write32(cursor, output);
    auto serialize = ctx; serialize.r3.u64 = state + 7668; serialize.r4.u64 = cursor;
    sub_821A8C48(serialize, base);
    check(PPC_LOAD_U8(output) == 180 && PPC_LOAD_U8(output + 1) == 0 &&
          PPC_LOAD_U8(output + 2) == 0xA5 && PPC_LOAD_U8(output + 3) == 0xA5 && memory->read32(cursor) == output + 4,
          "Original progression serialization must retain max hearts and preserve adjacent upgrade flags");
    PPC_STORE_U16(state + 7672, 0);
    memory->write32(cursor, output);
    auto deserialize = ctx; deserialize.r3.u64 = state + 7668; deserialize.r4.u64 = cursor;
    sub_821A8C98(deserialize, base);
    check(PPC_LOAD_U16(state + 7672) == 180 && getLevel() == 4, "Original save helper must roundtrip the maximum Darkness grant");

    prepare(); before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, true, true).applied, "Both queued Darkness actions must compose in one update");
    onlyChanged(before, true, true);
    before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, true, true).applied && before == snapshot(),
          "Repeated grants must be idempotent rather than incrementing hearts or compounding flags");

    PPC_STORE_U16(state + 7672, 300);
    PPC_STORE_U8(state + 8553, 120); PPC_STORE_U8(state + 7656, 130);
    memory->write32(state + 7076, 0x80000201);
    before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, false, true).applied && before == snapshot(),
          "A maximum-level grant must not lower existing hearts, bonus capacity or bonus energy");

    const auto rejected = [&](const DeveloperPlayerHandles& handles, uint8_t* candidateBase) {
        const auto result = applyDeveloperDarkness(ctx, candidateBase, handles, true, true);
        check(!result.applied && !result.status.empty() && before == snapshot(), "Invalid/stale player handles reached Darkness progression writes");
    };
    rejected({}, base); rejected(player, nullptr);
    auto stale = player; stale.state += 4; rejected(stale, base);
    stale = player; stale.server = fixture.client; rejected(stale, base);
    stale = player; stale.state = 0xFFFFF000; rejected(stale, base);
    stale = player; stale.actor += 1; rejected(stale, base);
    DWORD previous = 0;
    check(VirtualProtect(base + state, 0x3000, PAGE_READONLY, &previous), "Darkness fixture read-only protection");
    rejected(player, base);
    DWORD ignored = 0;
    check(VirtualProtect(base + state, 0x3000, previous, &ignored), "Darkness fixture protection restoration");
    check(VirtualProtect(base + state + 0x2000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &previous), "Darkness fixture guard protection");
    // Do not snapshot guarded bytes until their original protection returns.
    const auto guarded = applyDeveloperDarkness(ctx, base, player, true, true);
    check(!guarded.applied, "Guarded Darkness capacity/state cannot be accessed by a grant");
    check(VirtualProtect(base + state + 0x2000, 0x1000, previous, &ignored), "Darkness guard restoration");
    check(before == snapshot(), "Rejected guard-page grant modified other progression fields");

    // Exercise the actual UI-to-engine queue, including its ordering against
    // reset/mission/preload. No mission is launched: false mission readiness
    // deliberately reaches the ordinary rejected-command branch.
    auto engine = ctx;
    fixture.prepare(engine);
    const auto engineBefore = engine;
    struct ResetRequests {
        ~ResetRequests() { resetDeveloperTools(); }
    } resetRequests;
    const auto tick = [&](uint32_t client) {
        check(!processDeveloperTools(engine, base, client, false), "Darkness fixture must never launch a mission");
        check(std::memcmp(&engine, &engineBefore, sizeof(engine)) == 0,
              "Developer request processing changed its caller's guest registers");
        check(memory->read32(fixture.gameContext + 4) == 1,
              "Queued Darkness processing leaked the original GAMECONTEXT reference");
    };
    resetDeveloperTools(); prepare();
    requestDeveloperUnlockDarkness(); requestDeveloperMaxDarkness();
    tick(fixture.client);
    check(PPC_LOAD_U8(state + 7660) == 0xBF && PPC_LOAD_U8(state + 7659) == 0x7F &&
          PPC_LOAD_U16(state + 7672) == 180 && PPC_LOAD_U8(state + 8553) == 100 &&
          developerSnapshot().hasActivePlayer && developerSnapshot().status.find("autosaved") != std::string::npos,
          "Both queued grants must apply together to the original current player and publish their save implication");
    prepare(); before = snapshot();
    tick(fixture.client);
    check(before == snapshot(), "A consumed Darkness grant must not run again on the next engine update");

    requestDeveloperUnlockDarkness(); requestDeveloperMaxDarkness();
    resetDeveloperTools();
    tick(fixture.client);
    check(before == snapshot(), "Reset must discard unprocessed grants before the next engine update");

    requestDeveloperUnlockDarkness(); requestDeveloperMaxDarkness();
    check(requestDeveloperMission("NY1_Tunnel:ny1+layer1"), "Darkness queue mission fixture request");
    tick(fixture.client);
    check(before == snapshot() && developerSnapshot().status.starts_with("Mission loading is unavailable."),
          "A mission command must consume/drop concurrent Darkness grants even when the mission is rejected");
    tick(fixture.client);
    check(before == snapshot(), "Dropped mission-associated grants must not reach a later player/world update");

    requestDeveloperUnlockDarkness(); requestDeveloperMaxDarkness();
    tick(0);
    check(before == snapshot() && !developerSnapshot().hasActivePlayer &&
          developerSnapshot().status == "Darkness grants require an active player.",
          "An unavailable player must reject queued grants without retaining them");
    tick(fixture.client);
    check(before == snapshot(), "Rejected grants must not apply when a different/later player becomes available");

    requestDeveloperUnlockDarkness(); requestDeveloperMaxDarkness();
    PPC_STORE_U8(fixture.gameContext + 4908, 1);
    tick(fixture.client);
    check(before == snapshot() && !developerSnapshot().hasActivePlayer,
          "Original preloading must suppress grants to the outgoing player");
    PPC_STORE_U8(fixture.gameContext + 4908, 0);
    tick(fixture.client);
    check(before == snapshot(), "A preloading rejection must not retarget a queued grant after a transition");
    check(std::memcmp(base + fixture.cameraState, npcBefore.data(), npcBefore.size()) == 0,
          "Request-queue processing modified the camera NPC");
    puts("Developer Darkness: original thresholds/save roundtrip, six powers, player isolation and memory guards, one-shot queue/reset/mission/unavailable/preload consumption passed.");
}
