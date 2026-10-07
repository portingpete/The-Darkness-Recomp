#pragma once
#include "runtime/native/developer_darkness.h"
#include "runtime/native/developer_tools.h"
#include "developer_player_lookup_tests.h"
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace DeveloperDarknessFixture {
constexpr uint32_t grantAddress = 0x8218B4C0;
constexpr std::array<uint32_t, 2> ancientNames{0x82062D00, 0x82062D14};
inline uint32_t actor, state, inventory, itemArray, itemData;
inline std::array<uint32_t, 2> weapons;
inline unsigned grants;
inline uint32_t failName;

// Substitute the game world's item-creation boundary, since the full registry,
// animation and replication services are not started in NativeTests. The actual
// retail inventory lookup, category traversal and CStr comparison still run.
static PPC_FUNC(grant) {
    check(ctx.r3.u32 == actor && ctx.r4.u32 == 0 && ctx.r5.u32 == 0 &&
          ctx.r7.s64 == int16_t(PPC_LOAD_U16(actor + 368)) && ctx.r8.u32 == 0 && ctx.r9.u32 == 0,
          "Darkness gun grant changed the original giveall item-helper ABI");
    check(ctx.r6.u32 == ancientNames[0] || ctx.r6.u32 == ancientNames[1],
          "Darkness unlock attempted to grant another weapon, story item or Darkling");
    ++grants;
    // A nonzero helper return does not prove an inventory item was created.
    if (ctx.r6.u32 == failName) { ctx.r3.u64 = 1; return; }
    const auto weapon = weapons[ctx.r6.u32 == ancientNames[1]];
    const auto count = memory->read32(itemArray + 4);
    check(count < 4, "Darkness unlock redundantly invoked the original item helper");
    memory->write32(itemData + count * 4, weapon);
    memory->write32(itemArray + 4, count + 1);
    memory->write32(state + 7076, memory->read32(state + 7076) | 0x20000000);
    ctx.r3.u64 = 0; ctx.r31.u64 = 0xFEEDFACE;
}

struct Inventory {
    uint8_t* base;
    uint32_t unrelated;
    PPCFunc* original;
    explicit Inventory(const DeveloperPlayerLookupFixture::Fixture& player)
        : base(player.base), unrelated(player.block + 0x1B000), original(PPC_LOOKUP_FUNC(base, grantAddress)) {
        actor = player.actor; state = player.state; inventory = player.block + 0x19000;
        itemArray = player.block + 0x1A500; itemData = player.block + 0x1A600;
        weapons = {player.block + 0x1B400, player.block + 0x1B800};
        const auto categories = player.block + 0x19300, categoryData = player.block + 0x19380;
        memory->write32(actor + 656, inventory);
        memory->write32(inventory + 16, player.server);
        memory->write32(inventory + 24, categories);
        memory->write32(categories + 4, 4); memory->write32(categories + 24, categoryData);
        for (unsigned i = 0; i < 4; ++i) {
            const auto category = player.block + 0x19400 + i * 0x100;
            memory->write32(categoryData + i * 4, category);
            if (!i) memory->write32(category + 24, itemArray);
        }
        memory->write32(itemArray + 24, itemData);
        const auto name = [&](uint32_t item, uint32_t backing, const char* value, uint32_t type) {
            memory->write32(item + 8, 0x82065568); memory->write32(item + 12, backing);
            PPC_STORE_U16(backing, 0x4001);
            std::strcpy(reinterpret_cast<char*>(base + backing + 2), value);
            memory->write32(item + 320, type);
        };
        name(unrelated, player.block + 0x1C000, "weapon_fixture_ordinary", 1);
        for (unsigned i = 0; i < weapons.size(); ++i)
            name(weapons[i], player.block + 0x1C100 + i * 0x100,
                 reinterpret_cast<char*>(base + ancientNames[i]), 7);
        PPC_LOOKUP_FUNC(base, grantAddress) = grant;
        reset();
    }
    void reset() const {
        grants = 0; failName = 0;
        memory->write32(itemArray + 4, 1); memory->write32(itemData, unrelated);
        for (const auto item : weapons) memory->write32(item + 580, 0);
    }
    uint32_t lookup(PPCContext& ctx, uint32_t name) const {
        auto call = ctx; call.r3.u64 = inventory; call.r4.u64 = name;
        sub_823241C0(call, base);
        return call.r3.u32;
    }
    ~Inventory() { PPC_LOOKUP_FUNC(base, grantAddress) = original; }
};
}

static void testDeveloperDarkness(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    DeveloperPlayerLookupFixture::Fixture fixture;
    auto* base = fixture.base;
    const auto state = fixture.state;
    const auto player = resolveDeveloperPlayer(ctx, base, fixture.client);
    check(player.actor == fixture.actor && player.state == state, "Darkness fixture must resolve the original local player");
    const auto originalContext = ctx;
    DeveloperDarknessFixture::Inventory guns(fixture);
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
    for (unsigned i = 0; i < DeveloperDarknessFixture::ancientNames.size(); ++i)
        check(std::string_view(reinterpret_cast<char*>(base + DeveloperDarknessFixture::ancientNames[i])) ==
              (i ? "weapon_Ancient_2" : "weapon_Ancient_1"),
              "Ancient weapon grant must use the original giveall's concrete template names");
    // Both weapons share inventory type7. The retail name oracle must still
    // distinguish them and ignore an item carrying its original removed flag.
    memory->write32(DeveloperDarknessFixture::itemArray + 4, 2);
    memory->write32(DeveloperDarknessFixture::itemData + 4, DeveloperDarknessFixture::weapons[0]);
    check(guns.lookup(ctx, 0x82062D00) == DeveloperDarknessFixture::weapons[0] &&
          !guns.lookup(ctx, 0x82062D14), "Original inventory name lookup confused the two Ancient weapons");
    memory->write32(DeveloperDarknessFixture::weapons[0] + 580, 0x80000);
    check(!guns.lookup(ctx, 0x82062D00), "Original inventory name lookup accepted a removed Ancient weapon");
    guns.reset();
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
        guns.reset();
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
    check(memory->read32(state + 7076) == 0xA0000301, "Power grant must retain the original inventory helper's dirty flags and add the power-update flag");
    check(DeveloperDarknessFixture::grants == 2 && guns.lookup(ctx, 0x82062D00) && guns.lookup(ctx, 0x82062D14) &&
          memory->read32(DeveloperDarknessFixture::itemArray + 4) == 3 &&
          memory->read32(DeveloperDarknessFixture::itemData) == guns.unrelated,
          "Power unlock must grant both Ancient templates exactly once and preserve ordinary inventory");
    onlyChanged(before, true, false);

    prepare(); before = snapshot();
    const auto maximum = applyDeveloperDarkness(ctx, base, player, false, true);
    check(maximum.applied && PPC_LOAD_U16(state + 7672) == 180 && getLevel() == 4,
          "Maximum Darkness grant must reach the original engine's displayed level5");
    check(PPC_LOAD_U8(state + 7656) == 100 && PPC_LOAD_U8(state + 8553) == 100,
          "Maximum Darkness grant must match original level5 capacity/current energy");
    check(PPC_LOAD_U8(state + 7659) == 0x40 && PPC_LOAD_U8(state + 7660) == 0x80,
          "Maximum Darkness level must not also unlock powers");
    check(!DeveloperDarknessFixture::grants, "Maximum Darkness level must not invoke an inventory grant");
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
    check(applyDeveloperDarkness(ctx, base, player, true, true).applied && before == snapshot() &&
          DeveloperDarknessFixture::grants == 2,
          "Repeated grants must be idempotent rather than incrementing hearts or compounding flags");

    prepare();
    memory->write32(DeveloperDarknessFixture::itemArray + 4, 2);
    memory->write32(DeveloperDarknessFixture::itemData + 4, DeveloperDarknessFixture::weapons[0]);
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied && DeveloperDarknessFixture::grants == 1 &&
          guns.lookup(ctx, 0x82062D00) && guns.lookup(ctx, 0x82062D14),
          "Unlock must repair only a missing Ancient partner, even though both use item type7");

    prepare(); before = snapshot();
    DeveloperDarknessFixture::failName = 0x82062D14;
    const auto incomplete = applyDeveloperDarkness(ctx, base, player, true, true);
    check(!incomplete.applied && incomplete.status.find("incomplete") != std::string::npos &&
          guns.lookup(ctx, 0x82062D00) && !guns.lookup(ctx, 0x82062D14) &&
          PPC_LOAD_U8(state + 7660) == 0x80 && PPC_LOAD_U8(state + 7659) == 0x40 &&
          PPC_LOAD_U16(state + 7672) == 14 && PPC_LOAD_U8(state + 8553) == 20 &&
          memory->read32(state + 7076) == 0xA0000201,
          "Failed partner creation must preserve helper updates without reporting full success or changing powers/level");
    onlyChanged(before, false, false);
    DeveloperDarknessFixture::failName = 0;
    check(applyDeveloperDarkness(ctx, base, player, true, true).applied && DeveloperDarknessFixture::grants == 3,
          "Retry must retain the successful gun and request only the previously failed partner");

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
    memory->write32(fixture.actor + 656, 0); rejected(player, base);
    memory->write32(fixture.actor + 656, DeveloperDarknessFixture::inventory);
    memory->write32(DeveloperDarknessFixture::itemArray + 4, 0xFFFFFFFF); rejected(player, base);
    memory->write32(DeveloperDarknessFixture::itemArray + 4, 3);
    memory->write32(DeveloperDarknessFixture::itemArray + 24, 0xFFFFFFFC); rejected(player, base);
    memory->write32(DeveloperDarknessFixture::itemArray + 24, DeveloperDarknessFixture::itemData);
    const auto backing = memory->read32(guns.unrelated + 12);
    memory->write32(guns.unrelated + 12, 0xFFFFFFFF); rejected(player, base);
    memory->write32(guns.unrelated + 12, backing);
    auto noStack = ctx; noStack.r1.u64 = 0;
    check(!applyDeveloperDarkness(noStack, base, player, true, true).applied && before == snapshot(),
          "Missing guest stack must reject an inventory grant before any progression writes");
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
    puts("Developer Darkness: original thresholds/save/name lookup, Ancient helper ABI/dirty flags/partial repair/idempotence, six powers, memory guards and one-shot request consumption passed.");
}
