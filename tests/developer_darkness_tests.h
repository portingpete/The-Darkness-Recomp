#pragma once
#include "runtime/native/developer_darkness.h"
#include "runtime/native/developer_tools.h"
#include "developer_player_lookup_tests.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace DeveloperDarknessFixture {
constexpr uint32_t grantAddress = 0x8218B4C0;
constexpr std::array<uint32_t, 2> ancientNames{0x82062D00, 0x82062D14};
constexpr uint32_t resourceFlags = 0xA5000021;
constexpr uint32_t resolveAddress = 0x823328E8, releaseAddress = 0x820CBBC8;
constexpr uint32_t precacheAddress = 0x822DCDD8, evaluateAddress = 0x822DD490;
constexpr uint32_t countAddress = 0x82334368, keyAddress = 0x82334240, hashAddress = 0x823342D8;
constexpr std::array<uint32_t, 8> boundaries{
    grantAddress, resolveAddress, releaseAddress, precacheAddress, evaluateAddress,
    countAddress, keyAddress, hashAddress};
inline uint32_t actor, state, inventory, itemArray, itemData, resources, server;
inline uint32_t registry, registryTable, keyTable, keys, effectData, clone, cloneCategory, cloneArray, cloneData;
inline std::array<uint32_t, 2> weapons;
inline unsigned grants, resolutions, precaches, releases, templateReferences;
inline uint32_t failName, failTemplate, failHash;
inline std::vector<uint32_t> evaluated;
constexpr std::array<uint32_t, 6> assetKeys{
    0x0FF1EEF1, 0x713CE1A9, 0x86314D73, 0x1562BFDF, 0xA7C1B409, 0xDEADBEEF};

static void resourcesOpen() {
    check(memory->read32(resources + 24) == (resourceFlags & ~1u),
          "Late Darkness creation and repair must permit resource additions and retain unrelated context flags");
}
static void readyWeapon(uint32_t weapon) {
    auto* base = memory->base();
    memory->write32(weapon, 0x8206C5B0);
    memory->write32(weapon + 16, server);
    memory->write32(weapon + 340, 0x301); memory->write32(weapon + 344, 0x401);
    PPC_STORE_U16(weapon + 466, 0x101); PPC_STORE_U16(weapon + 636, 0x201);
    memory->write32(weapon + 896, 0xFFFFFFFF);
}
static bool ancientItem(uint32_t item) { return item == weapons[0] || item == weapons[1] || item == clone; }
static PPC_FUNC(resolveTemplate) {
    resourcesOpen();
    check((ctx.r4.u32 == ancientNames[0] || ctx.r4.u32 == ancientNames[1]) && ctx.r5.u32 == server,
          "Ancient resource repair must resolve only its original template through the owning server");
    ++resolutions;
    memory->write32(ctx.r3.u32, ctx.r4.u32 == failTemplate ? 0 : registry);
    if (ctx.r4.u32 != failTemplate) ++templateReferences;
    ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(releaseTemplate) {
    resourcesOpen();
    const auto reference = memory->read32(ctx.r3.u32);
    check(!reference || reference == registry, "Ancient repair released an unrelated registry reference");
    if (reference) {
        check(templateReferences != 0, "Ancient repair released its template twice");
        --templateReferences;
    }
    memory->write32(ctx.r3.u32, 0); ++releases;
    ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(precache) {
    resourcesOpen();
    check(ancientItem(ctx.r3.u32) && ctx.r4.u32 == registry &&
          ctx.r5.u32 == resources && ctx.r6.u32 == server,
          "Ancient repair changed the original template-precache ABI");
    ++precaches;
    ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(keyCount) {
    check(ctx.r3.u32 == registry, "Ancient repair enumerated an unrelated registry");
    ctx.r3.u64 = assetKeys.size();
}
static PPC_FUNC(keyAt) {
    check(ctx.r3.u32 == registry && ctx.r4.u32 < assetKeys.size(), "Ancient repair read outside template keys");
    ctx.r3.u64 = keys + ctx.r4.u32 * 16;
}
static PPC_FUNC(keyHash) {
    check(ctx.r3.u32 >= keys && ctx.r3.u32 < keys + assetKeys.size() * 16 && !(ctx.r3.u32 & 15),
          "Ancient repair requested an unrelated registry key hash");
    ctx.r3.u64 = memory->read32(ctx.r3.u32 + 4);
}
static PPC_FUNC(evaluate) {
    resourcesOpen();
    const auto item = ctx.r3.u32, hash = ctx.r4.u32;
    check(ancientItem(item) && hash != 0xDEADBEEF &&
          memory->read32(ctx.r5.u32 + 4) == hash,
          "Ancient repair replayed gameplay properties or changed the original resource-key ABI");
    evaluated.push_back(hash);
    if (hash == failHash) return;
    if (hash == 0x0FF1EEF1 || hash == 0x0E326DE1) PPC_STORE_U16(item + 466, 0x101);
    else if (hash == 0x713CE1A9) PPC_STORE_U16(item + 636, 0x201);
    else if (hash == 0x86314D73) memory->write32(item + 340, 0x301);
    else if (hash == 0x1562BFDF || hash == 0xC5F42FDC) memory->write32(item + 344, 0x401);
    else if (hash == 0xA7C1B409) memory->write32(item + 504, effectData);
    else check(false, "Ancient repair replayed an unexpected key");
    ctx.r31.u64 = 0xFEEDFACE;
}

// Substitute the game world's item-creation boundary, since the full registry,
// animation and replication services are not started in NativeTests. The actual
// retail inventory lookup, category traversal and CStr comparison still run.
static PPC_FUNC(grant) {
    resourcesOpen();
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
    std::array<PPCFunc*, boundaries.size()> originals;
    explicit Inventory(const DeveloperPlayerLookupFixture::Fixture& player)
        : base(player.base), unrelated(player.block + 0x1B000) {
        actor = player.actor; state = player.state; inventory = player.block + 0x19000;
        server = player.server;
        resources = player.block + 0x1D000;
        memory->write32(player.server + 432, resources);
        memory->write32(resources, 0x82070780);
        itemArray = player.block + 0x1A500; itemData = player.block + 0x1A600;
        weapons = {player.block + 0x1B400, player.block + 0x1B800};
        registry = player.block + 0x1E000; registryTable = player.block + 0x1E400;
        keys = player.block + 0x1F000; keyTable = player.block + 0x1F400;
        effectData = player.block + 0x2A000;
        cloneArray = player.block + 0x2B100; cloneData = player.block + 0x2B200;
        clone = player.block + 0x2B400;
        memory->write32(registry, registryTable);
        memory->write32(registryTable + 52, countAddress); memory->write32(registryTable + 64, keyAddress);
        memory->write32(keyTable + 428, hashAddress);
        const auto categories = player.block + 0x19300, categoryData = player.block + 0x19380;
        memory->write32(actor + 656, inventory);
        memory->write32(inventory + 16, player.server);
        memory->write32(inventory + 24, categories);
        memory->write32(categories + 4, 4); memory->write32(categories + 24, categoryData);
        for (unsigned i = 0; i < 4; ++i) {
            const auto category = player.block + 0x19400 + i * 0x100;
            memory->write32(categoryData + i * 4, category);
            if (!i) memory->write32(category + 24, itemArray);
            if (i == 2) {
                cloneCategory = category;
                memory->write32(category + 24, cloneArray);
                memory->write32(cloneArray + 24, cloneData);
                memory->write32(cloneData, clone);
            }
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
        name(clone, player.block + 0x2C000, reinterpret_cast<char*>(base + ancientNames[0]), 7 | 0x200);
        const std::array<PPCFunc*, boundaries.size()> substitutes{
            grant, resolveTemplate, releaseTemplate, precache, evaluate, keyCount, keyAt, keyHash};
        for (unsigned i = 0; i < boundaries.size(); ++i) {
            originals[i] = PPC_LOOKUP_FUNC(base, boundaries[i]);
            PPC_LOOKUP_FUNC(base, boundaries[i]) = substitutes[i];
        }
        reset();
    }
    void reset() const {
        grants = resolutions = precaches = releases = templateReferences = 0;
        failName = failTemplate = failHash = 0; evaluated.clear();
        memory->write32(resources + 24, resourceFlags);
        memory->write32(itemArray + 4, 1); memory->write32(itemData, unrelated);
        for (unsigned i = 0; i < assetKeys.size(); ++i) {
            memory->write32(keys + i * 16, keyTable);
            memory->write32(keys + i * 16 + 4, assetKeys[i]);
        }
        for (unsigned i = 0; i < weapons.size(); ++i) {
            const auto item = weapons[i]; readyWeapon(item);
            memory->write32(item + 580, 0); memory->write32(item + 628, 100 + i);
        }
        readyWeapon(clone); memory->write32(clone + 580, 0); memory->write32(clone + 628, 301);
        memory->write32(cloneArray + 4, 0); memory->write32(cloneCategory + 48, 0);
    }
    uint32_t lookup(PPCContext& ctx, uint32_t name) const {
        auto call = ctx; call.r3.u64 = inventory; call.r4.u64 = name;
        sub_823241C0(call, base);
        return call.r3.u32;
    }
    ~Inventory() {
        for (unsigned i = 0; i < boundaries.size(); ++i) PPC_LOOKUP_FUNC(base, boundaries[i]) = originals[i];
    }
};
}

namespace DeveloperDarknessClientFixture {
constexpr uint32_t growAddress = 0x82346CE0, nameAddress = 0x82342548;
constexpr uint32_t registerAddress = 0x82342600, warmAddress = 0x823E5F08;
constexpr std::array<uint32_t, 4> boundaries{growAddress, nameAddress, registerAddress, warmAddress};
constexpr uint32_t firstModel = 0x101, secondModel = 0x102, laterModel = 0x103;
constexpr uint32_t earlyModel = 0x100;
constexpr uint32_t blacklistedModel = 0x104, sourceCount = 0x106;
constexpr uint32_t clientFlags = 0xA6000021;
inline uint32_t client, serverContext, clientContext, serverArray, clientArray, serverData, clientData;
inline uint32_t provider, renderer, engineTable, sourcePrefix, targetPrefix, stale;
inline uint32_t earlyEntry, earlyName;
inline std::array<uint32_t, 4> models, nameBackings;
inline unsigned grows, names;
inline uint32_t failIndex;
inline std::vector<uint32_t> registrations, warmed;

static uint32_t source(uint32_t index) { return memory->read32(serverData + index * 4); }
static uint32_t nameBacking(uint32_t index) {
    return index == earlyModel ? earlyName : nameBackings[index - firstModel];
}
static PPC_FUNC(grow) {
    check(ctx.r3.u32 == clientContext + 8 && ctx.r4.u32 == sourceCount,
          "Client resource sync changed the original preserving-growth ABI");
    check(memory->read32(clientContext + 12) == clientArray,
          "Client resource sync replaced its existing resource TArray");
    const auto oldCount = memory->read32(clientArray + 4);
    check(oldCount <= sourceCount, "Client resource sync tried to shrink or replace its resource array");
    for (uint32_t i = oldCount; i < sourceCount; ++i) memory->write32(clientData + i * 4, 0);
    memory->write32(clientArray + 4, sourceCount);
    ++grows; ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(name) {
    check(ctx.r4.u32 == serverContext && ctx.r5.u32 >= earlyModel && ctx.r5.u32 <= laterModel,
          "Client resource sync requested a name outside the server's eligible late resources");
    const auto output = ctx.r3.u32;
    const auto backing = nameBacking(ctx.r5.u32);
    check(PPC_LOAD_U16(backing) == 0x4001, "Client resource name leaked a previous temporary CStr reference");
    // Retail resource+28 returns an owning CStr. Retain its fixture backing so
    // original821F8AD0 can drop that reference without freeing the allocation.
    PPC_STORE_U16(backing, 0x4002);
    memory->write32(output, 0x82065568);
    memory->write32(output + 4, backing);
    ++names; ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(registerResource) {
    const auto index = ctx.r4.u32;
    check(ctx.r3.u32 == clientContext && index >= earlyModel && index <= laterModel &&
          ctx.r5.u32 == nameBacking(index) + 2 && ctx.r6.u32 == 3,
          "Client resource sync changed fixed-ID registration or serialized XMD type3 ABI");
    check(memory->read32(clientData + index * 4) == 0,
          "Client resource sync overwrote an existing client resource instead of preserving it");
    check(memory->read32(clientContext + 24) == clientFlags,
          "Client resource sync changed the client's unrelated resource flags");
    registrations.push_back(index);
    if (index != failIndex) memory->write32(clientData + index * 4, source(index));
    ctx.r31.u64 = 0xFEEDFACE;
}
static PPC_FUNC(warm) {
    const auto found = std::find(models.begin(), models.begin() + 3, ctx.r3.u32);
    check((found != models.begin() + 3 || ctx.r3.u32 == earlyEntry) && ctx.r4.u32 == renderer,
          "Ancient model warming must call the original CWRes_Model boundary with the actual client CXR context");
    const auto activeContext = memory->read32(client + 432);
    check(memory->read32(activeContext + 24) == (activeContext == serverContext ?
              (DeveloperDarknessFixture::resourceFlags & ~1u) : clientFlags),
          "Model warming changed client resource-context flags");
    warmed.push_back(ctx.r3.u32 == earlyEntry ? earlyModel : firstModel + uint32_t(found - models.begin()));
    ctx.r31.u64 = 0xFEEDFACE;
}

// Substitute only service boundaries requiring a running renderer/provider.
// Retail resource descriptors, CStr raw-name access and destruction remain
// original, including the XMD serialized type3 versus its model base class.
struct Resources {
    uint8_t* base;
    std::array<PPCFunc*, boundaries.size()> originals;
    explicit Resources(const DeveloperPlayerLookupFixture::Fixture& player) : base(player.base) {
        client = player.client; serverContext = DeveloperDarknessFixture::resources;
        serverArray = player.block + 0x2D000; serverData = player.block + 0x2D100;
        clientContext = player.block + 0x2E000; clientArray = player.block + 0x2E100;
        clientData = player.block + 0x2E200; provider = player.block + 0x2F000;
        models = {player.block + 0x30000, player.block + 0x30200,
                  player.block + 0x30600, player.block + 0x30800};
        nameBackings = {player.block + 0x30400, player.block + 0x30500,
                        player.block + 0x30700, player.block + 0x30900};
        engineTable = player.block + 0x30A00;
        renderer = player.block + 0x30E00;
        earlyEntry = player.block + 0x31000; earlyName = player.block + 0x31200;
        sourcePrefix = player.block + 0x30B00; targetPrefix = player.block + 0x30C00;
        stale = player.block + 0x30D00;
        const std::array<PPCFunc*, boundaries.size()> substitutes{grow, name, registerResource, warm};
        for (unsigned i = 0; i < boundaries.size(); ++i) {
            originals[i] = PPC_LOOKUP_FUNC(base, boundaries[i]);
            PPC_LOOKUP_FUNC(base, boundaries[i]) = substitutes[i];
        }
        for (unsigned i = 0; i < models.size(); ++i) {
            memory->write32(models[i], 0x8207E5C0);
            memory->write32(models[i] + 16, 0x03030000 | (firstModel + i));
            PPC_STORE_U16(nameBackings[i], 0x4001);
            std::strcpy(reinterpret_cast<char*>(base + nameBackings[i] + 2),
                        i == 0 ? "gun_darkness_01" : i == 1 ? "gun_darkness_02" : "fixture_late_model");
        }
        memory->write32(models[3] + 16, memory->read32(models[3] + 16) | 0x10000000);
        memory->write32(earlyEntry, 0x8207E5C0); memory->write32(earlyEntry + 16, 0x03030100);
        PPC_STORE_U16(earlyName, 0x4001);
        std::strcpy(reinterpret_cast<char*>(base + earlyName + 2), "fixture_projectile_dependency");
        memory->write32(provider, 0x820818D8);
        memory->write32(engineTable + 48, 0x827AAB08);
        memory->write32(engineTable + 52, 0x827AAB08);
        reset();
    }
    void reset() const {
        grows = names = 0; failIndex = 0; registrations.clear(); warmed.clear();
        memory->write32(client, 0x82081BF0); memory->write32(client + 516, 0);
        memory->write32(client + 536, 3); memory->write32(client + 432, clientContext);
        memory->write32(client + 600, renderer); memory->write32(renderer, engineTable);
        memory->write32(serverContext + 12, serverArray); memory->write32(serverContext + 80, provider);
        memory->write32(serverArray + 4, sourceCount); memory->write32(serverArray + 24, serverData);
        memory->write32(clientContext, 0x82070780);
        memory->write32(clientContext + 12, clientArray); memory->write32(clientContext + 24, clientFlags);
        memory->write32(clientContext + 80, provider);
        memory->write32(clientArray + 4, firstModel); memory->write32(clientArray + 24, clientData);
        std::memset(base + serverData, 0, sourceCount * 4);
        std::memset(base + clientData, 0, sourceCount * 4);
        memory->write32(serverData, sourcePrefix); memory->write32(clientData, targetPrefix);
        for (unsigned i = 0; i < models.size(); ++i) memory->write32(serverData + (firstModel + i) * 4, models[i]);
    }
    void distinctGunModels() const {
        PPC_STORE_U16(DeveloperDarknessFixture::weapons[0] + 466, firstModel);
        PPC_STORE_U16(DeveloperDarknessFixture::weapons[1] + 466, secondModel);
    }
    ~Resources() {
        for (unsigned i = 0; i < boundaries.size(); ++i) PPC_LOOKUP_FUNC(base, boundaries[i]) = originals[i];
    }
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
    DeveloperDarknessClientFixture::Resources clientResources(fixture);
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
        clientResources.reset();
        std::memset(base + state, 0xA5, 10244);
        memory->write32(state + 7076, 0x80000201);
        PPC_STORE_U8(state + 7656, 17);
        PPC_STORE_U8(state + 7659, 0x40);
        PPC_STORE_U8(state + 7660, 0x80);
        PPC_STORE_U16(state + 7672, 14);
        PPC_STORE_U8(state + 8553, 20);
    };
    const auto onlyChanged = [&](const std::vector<uint8_t>& before, bool unlock, bool maximum) {
        check(memory->read32(DeveloperDarknessFixture::resources + 24) == DeveloperDarknessFixture::resourceFlags,
              "Darkness grant must restore the exact resource-context flags after success or failure");
        check(DeveloperDarknessFixture::templateReferences == 0,
              "Darkness resource repair must release its original template reference on every exit");
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
          DeveloperDarknessFixture::grants == 2 &&
          memory->read32(DeveloperDarknessFixture::resources + 24) == DeveloperDarknessFixture::resourceFlags,
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

    // Model+466, AG2+636 and EFFECTLIST contain resolved resources, while the
    // identity, ammunition, equipped replica and progression fields remain
    // gameplay state. Repair old zero-resource items in place through the same
    // retail template boundaries used by item creation.
    const auto addExisting = [&] {
        memory->write32(DeveloperDarknessFixture::itemArray + 4, 3);
        for (unsigned i = 0; i < DeveloperDarknessFixture::weapons.size(); ++i)
            memory->write32(DeveloperDarknessFixture::itemData + 4 + i * 4, DeveloperDarknessFixture::weapons[i]);
    };
    const auto malformed = [&](uint32_t item) {
        memory->write32(item + 340, 0); memory->write32(item + 344, 0);
        PPC_STORE_U16(item + 466, 0); PPC_STORE_U16(item + 636, 0);
        memory->write32(item + 504, 0);
        PPC_STORE_U16(item + 396, 9); memory->write32(item + 584, 0x8000E419);
    };
    prepare(); addExisting();
    std::array<std::vector<uint8_t>, 2> itemsBefore;
    for (unsigned i = 0; i < DeveloperDarknessFixture::weapons.size(); ++i) {
        const auto item = DeveloperDarknessFixture::weapons[i]; malformed(item);
        itemsBefore[i] = std::vector<uint8_t>(base + item, base + item + 960);
    }
    before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
          !DeveloperDarknessFixture::grants && DeveloperDarknessFixture::precaches == 2 &&
          DeveloperDarknessFixture::resolutions == 2 && DeveloperDarknessFixture::releases == 2 &&
          DeveloperDarknessFixture::evaluated.size() == 10,
          "Existing malformed Ancient pair must repair its assets without duplicate grants");
    for (unsigned i = 0; i < DeveloperDarknessFixture::weapons.size(); ++i) {
        const auto item = DeveloperDarknessFixture::weapons[i];
        check(PPC_LOAD_U16(item + 466) == 0x101 && PPC_LOAD_U16(item + 636) == 0x201 &&
              memory->read32(item + 504) == DeveloperDarknessFixture::effectData,
              "Ancient repair must restore its true model, animation graph and effect resources");
        for (unsigned offset = 0; offset < itemsBefore[i].size(); ++offset) {
            const bool asset = (offset >= 340 && offset < 348) || (offset >= 466 && offset < 468) ||
                (offset >= 504 && offset < 508) || (offset >= 636 && offset < 638);
            check(asset || itemsBefore[i][offset] == base[item + offset],
                  "Ancient asset repair changed item identity, ammunition, flags or equipped replica state");
        }
    }
    onlyChanged(before, true, false);
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
          DeveloperDarknessFixture::resolutions == 2 && DeveloperDarknessFixture::evaluated.size() == 10,
          "Repeated unlock must retain repaired resources without replaying valid weapon templates");

    prepare(); addExisting(); before = snapshot();
    const auto replica = DeveloperDarknessFixture::clone;
    malformed(replica);
    memory->write32(DeveloperDarknessFixture::weapons[0] + 896, 301);
    memory->write32(DeveloperDarknessFixture::cloneArray + 4, 1);
    memory->write32(DeveloperDarknessFixture::cloneCategory + 48, 301);
    const auto cloneBefore = std::vector<uint8_t>(base + replica, base + replica + 960);
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
          !DeveloperDarknessFixture::grants && DeveloperDarknessFixture::precaches == 1 &&
          DeveloperDarknessFixture::evaluated.size() == 5 &&
          PPC_LOAD_U16(replica + 466) == 0x101 && PPC_LOAD_U16(replica + 636) == 0x201 &&
          memory->read32(replica + 628) == 301 && memory->read32(replica + 320) == (7 | 0x200) &&
          memory->read32(DeveloperDarknessFixture::weapons[0] + 896) == 301 &&
          memory->read32(DeveloperDarknessFixture::cloneCategory + 48) == 301 &&
          memory->read32(DeveloperDarknessFixture::cloneArray + 4) == 1,
          "Ancient repair must find the equipped category2 replica by its retail ID and retain equip bookkeeping");
    for (unsigned offset = 0; offset < cloneBefore.size(); ++offset) {
        const bool asset = (offset >= 340 && offset < 348) || (offset >= 466 && offset < 468) ||
            (offset >= 504 && offset < 508) || (offset >= 636 && offset < 638);
        check(asset || cloneBefore[offset] == base[replica + offset],
              "Equipped Ancient replica repair changed unrelated gameplay or identity fields");
    }
    onlyChanged(before, true, false);

    prepare(); addExisting(); before = snapshot();
    const auto partialItem = DeveloperDarknessFixture::weapons[0];
    PPC_STORE_U16(partialItem + 466, 0);
    PPC_STORE_U16(partialItem + 636, 0x555);
    memory->write32(partialItem + 340, 0xABCD); memory->write32(partialItem + 344, 0xBCDE);
    memory->write32(DeveloperDarknessFixture::keys + 4, 0x0E326DE1); // Original MODEL0 alias.
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
          DeveloperDarknessFixture::evaluated.size() == 2 &&
          DeveloperDarknessFixture::evaluated[0] == 0x0E326DE1 &&
          PPC_LOAD_U16(partialItem + 636) == 0x555 && memory->read32(partialItem + 340) == 0xABCD &&
          memory->read32(partialItem + 344) == 0xBCDE,
          "Partial asset repair must accept the retail model alias and retain already-valid resources");
    onlyChanged(before, true, false);

    prepare(); addExisting(); before = snapshot();
    PPC_STORE_U16(partialItem + 466, 0x444); PPC_STORE_U16(partialItem + 636, 0xFFFF);
    memory->write32(partialItem + 340, 0xABCD); memory->write32(partialItem + 344, 0xBCDE);
    memory->write32(partialItem + 504, 0);
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
          DeveloperDarknessFixture::evaluated.size() == 2 &&
          DeveloperDarknessFixture::evaluated[0] == 0x713CE1A9 &&
          PPC_LOAD_U16(partialItem + 636) == 0x201 && PPC_LOAD_U16(partialItem + 466) == 0x444 &&
          memory->read32(partialItem + 340) == 0xABCD && memory->read32(partialItem + 344) == 0xBCDE &&
          memory->read32(partialItem + 504) == DeveloperDarknessFixture::effectData,
          "Negative Ancient animation graph must repair its graph and effects while retaining valid model and sounds");
    onlyChanged(before, true, false);

    prepare(); addExisting(); before = snapshot(); malformed(DeveloperDarknessFixture::weapons[0]);
    DeveloperDarknessFixture::failHash = 0x0FF1EEF1;
    check(!applyDeveloperDarkness(ctx, base, player, true, true).applied &&
          !DeveloperDarknessFixture::grants && DeveloperDarknessFixture::releases == 1 &&
          before == snapshot(),
          "Unresolved required Ancient model must reject success and maximum progression while releasing its template");
    onlyChanged(before, false, false);
    DeveloperDarknessFixture::failHash = 0;
    check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
          !DeveloperDarknessFixture::grants && DeveloperDarknessFixture::releases == 2,
          "Retry must repair the existing partial asset result without replacing either Ancient item");

    for (const auto [offset, hash] : std::array<std::pair<uint32_t, uint32_t>, 2>{{
        {340, 0x86314D73}, {344, 0x1562BFDF}}}) {
        prepare(); addExisting(); before = snapshot();
        memory->write32(partialItem + offset, 0);
        DeveloperDarknessFixture::failHash = hash;
        check(!applyDeveloperDarkness(ctx, base, player, true, true).applied &&
              !DeveloperDarknessFixture::grants && DeveloperDarknessFixture::releases == 1 &&
              DeveloperDarknessFixture::evaluated == std::vector<uint32_t>{hash} &&
              !memory->read32(partialItem + offset) && before == snapshot(),
              "Unresolved Ancient firing cast must reject success without granting progression or replacing the item");
        onlyChanged(before, false, false);
        DeveloperDarknessFixture::failHash = 0;
        check(applyDeveloperDarkness(ctx, base, player, true, false).applied &&
              !DeveloperDarknessFixture::grants && DeveloperDarknessFixture::releases == 2 &&
              memory->read32(partialItem + offset),
              "Retry must repair the missing Ancient firing cast on the retained item");
        onlyChanged(before, true, false);
    }

    prepare(); addExisting(); before = snapshot(); malformed(DeveloperDarknessFixture::weapons[0]);
    DeveloperDarknessFixture::failTemplate = DeveloperDarknessFixture::ancientNames[0];
    check(!applyDeveloperDarkness(ctx, base, player, true, true).applied &&
          !DeveloperDarknessFixture::precaches && DeveloperDarknessFixture::releases == 1 && before == snapshot(),
          "Missing Ancient template must reject before asset evaluation or progression writes");
    onlyChanged(before, false, false);

    prepare(); addExisting(); before = snapshot();
    memory->write32(DeveloperDarknessFixture::weapons[0], 0x8206C5B4);
    check(!applyDeveloperDarkness(ctx, base, player, true, true).applied &&
          !DeveloperDarknessFixture::resolutions && before == snapshot(),
          "Ancient repair must reject a stale or unrelated weapon class before template evaluation");
    onlyChanged(before, false, false);
    prepare();

    // A late grant must synchronize fixed server resource IDs into the client
    // and warm their actual model base class, rather than merely registering
    // server metadata. Distinct model IDs also cover both hands independently.
    const auto prepareClient = [&] {
        prepare(); addExisting(); clientResources.distinctGunModels();
    };
    prepare(); clientResources.distinctGunModels(); before = snapshot();
    auto rendererLookup = ctx; rendererLookup.r3.u64 = fixture.client;
    sub_82499C38(rendererLookup, base);
    check(rendererLookup.r3.u32 == DeveloperDarknessClientFixture::renderer &&
          rendererLookup.r3.u32 != fixture.client + 600,
          "Original client renderer getter must return the pointer stored at+600, not the address of that field");
    check(applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied,
          "A late Ancient pair must prepare its client resources before reporting success");
    check(DeveloperDarknessClientFixture::grows == 1 && DeveloperDarknessClientFixture::names == 3 &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x101, 0x102, 0x103}) &&
          DeveloperDarknessClientFixture::warmed == std::vector<uint32_t>({0x101, 0x102, 0x103}) &&
          memory->read32(DeveloperDarknessClientFixture::clientArray + 4) == DeveloperDarknessClientFixture::sourceCount,
          "Client resource sync must preserve-grow once, register exact late IDs/type3, and warm each eligible model");
    check(memory->read32(DeveloperDarknessClientFixture::clientData) == DeveloperDarknessClientFixture::targetPrefix &&
          memory->read32(DeveloperDarknessClientFixture::serverData) == DeveloperDarknessClientFixture::sourcePrefix &&
          !memory->read32(DeveloperDarknessClientFixture::clientData + 0x104 * 4) &&
          !memory->read32(DeveloperDarknessClientFixture::clientData + 0x105 * 4) &&
          memory->read32(DeveloperDarknessClientFixture::clientContext + 24) == DeveloperDarknessClientFixture::clientFlags,
          "Client sync must retain its existing prefix/flags and skip blacklisted or absent source resources");
    for (unsigned i = 0; i < 3; ++i)
        check(memory->read32(DeveloperDarknessClientFixture::clientData + (0x101 + i) * 4) ==
                  DeveloperDarknessClientFixture::models[i] &&
              PPC_LOAD_U16(DeveloperDarknessClientFixture::nameBackings[i]) == 0x4001,
              "Fixed-ID registration must retain shared provider resource identity and release temporary retail CStr names safely");
    onlyChanged(before, true, true);
    before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied && before == snapshot() &&
          DeveloperDarknessFixture::grants == 2 && DeveloperDarknessClientFixture::grows == 1 &&
          DeveloperDarknessClientFixture::registrations.size() == 3 && DeveloperDarknessClientFixture::names == 3 &&
          DeveloperDarknessClientFixture::warmed.size() == 6,
          "Repeated client preparation may repeat retail warming but must not register resources or grant inventory twice");

    prepareClient(); before = snapshot();
    DeveloperDarknessClientFixture::failIndex = 0x102;
    const auto partialClient = applyDeveloperDarkness(ctx, base, player, true, true, fixture.client);
    check(!partialClient.applied && partialClient.status.find("client resources incomplete") != std::string::npos &&
          before == snapshot() && !DeveloperDarknessFixture::grants &&
          memory->read32(DeveloperDarknessClientFixture::clientData + 0x101 * 4) == DeveloperDarknessClientFixture::models[0] &&
          !memory->read32(DeveloperDarknessClientFixture::clientData + 0x102 * 4) &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x101, 0x102}) &&
          DeveloperDarknessClientFixture::warmed.empty(),
          "Partial fixed-ID registration must retain its first success without granting powers, level or reporting readiness");
    onlyChanged(before, false, false);
    DeveloperDarknessClientFixture::failIndex = 0;
    check(applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied &&
          DeveloperDarknessClientFixture::grows == 1 &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x101, 0x102, 0x102, 0x103}) &&
          DeveloperDarknessClientFixture::warmed == std::vector<uint32_t>({0x101, 0x102, 0x103}),
          "Retry after array growth must preserve the successful shared entry and fill only the remaining missing resources");
    onlyChanged(before, true, true);

    prepareClient(); before = snapshot();
    memory->write32(DeveloperDarknessClientFixture::serverData + 0x100 * 4, DeveloperDarknessClientFixture::earlyEntry);
    DeveloperDarknessClientFixture::failIndex = 0x100;
    check(!applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied && before == snapshot() &&
          DeveloperDarknessClientFixture::grows == 1 &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x100}) &&
          !memory->read32(DeveloperDarknessClientFixture::clientData + 0x100 * 4) &&
          DeveloperDarknessClientFixture::warmed.empty(),
          "An earlier projectile dependency failure must reject after preserving-growth without granting progression");
    onlyChanged(before, false, false);
    DeveloperDarknessClientFixture::failIndex = 0;
    check(applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied &&
          DeveloperDarknessClientFixture::grows == 1 &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x100, 0x100, 0x101, 0x102, 0x103}) &&
          DeveloperDarknessClientFixture::warmed == std::vector<uint32_t>({0x100, 0x101, 0x102, 0x103}) &&
          memory->read32(DeveloperDarknessClientFixture::clientData + 0x100 * 4) == DeveloperDarknessClientFixture::earlyEntry &&
          memory->read32(DeveloperDarknessClientFixture::clientData) == DeveloperDarknessClientFixture::targetPrefix &&
          PPC_LOAD_U16(DeveloperDarknessClientFixture::earlyName) == 0x4001,
          "Retry must find earlier holes after array growth and warm newly mirrored dependencies while preserving the old prefix");
    onlyChanged(before, true, true);

    prepareClient(); before = snapshot();
    memory->write32(DeveloperDarknessClientFixture::serverData + 0x100 * 4, DeveloperDarknessClientFixture::earlyEntry);
    DeveloperDarknessClientFixture::failIndex = 0x101;
    check(!applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied && before == snapshot() &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x100, 0x101}) &&
          memory->read32(DeveloperDarknessClientFixture::clientData + 0x100 * 4) == DeveloperDarknessClientFixture::earlyEntry &&
          DeveloperDarknessClientFixture::warmed.empty(),
          "A successful earlier dependency must survive a later registration failure without falsely reporting readiness");
    onlyChanged(before, false, false);
    DeveloperDarknessClientFixture::failIndex = 0;
    check(applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied &&
          DeveloperDarknessClientFixture::grows == 1 &&
          DeveloperDarknessClientFixture::registrations == std::vector<uint32_t>({0x100, 0x101, 0x101, 0x102, 0x103}) &&
          DeveloperDarknessClientFixture::warmed == std::vector<uint32_t>({0x100, 0x101, 0x102, 0x103}) &&
          PPC_LOAD_U16(DeveloperDarknessClientFixture::earlyName) == 0x4001,
          "Retry must warm pending earlier additions even when their slots are already filled and the client array has already grown");
    onlyChanged(before, true, true);
    before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied && before == snapshot() &&
          DeveloperDarknessClientFixture::registrations.size() == 5 &&
          DeveloperDarknessClientFixture::warmed == std::vector<uint32_t>({0x100, 0x101, 0x102, 0x103, 0x101, 0x102, 0x103}),
          "Successful preparation must clear earlier pending warming instead of retaining it across later grants");

    prepareClient(); before = snapshot();
    memory->write32(DeveloperDarknessClientFixture::clientArray + 4, DeveloperDarknessClientFixture::sourceCount);
    memory->write32(DeveloperDarknessClientFixture::clientData + 0x101 * 4, DeveloperDarknessClientFixture::stale);
    check(!applyDeveloperDarkness(ctx, base, player, true, true, fixture.client).applied && before == snapshot() &&
          memory->read32(DeveloperDarknessClientFixture::clientData + 0x101 * 4) == DeveloperDarknessClientFixture::stale &&
          DeveloperDarknessClientFixture::registrations.empty() && DeveloperDarknessClientFixture::warmed.empty(),
          "A conflicting live client ID must reject safely without overwriting or warming another resource");
    onlyChanged(before, false, false);

    prepareClient(); before = snapshot();
    memory->write32(fixture.client + 432, DeveloperDarknessClientFixture::serverContext);
    check(applyDeveloperDarkness(ctx, base, player, true, false, fixture.client).applied &&
          !DeveloperDarknessClientFixture::grows && !DeveloperDarknessClientFixture::names &&
          DeveloperDarknessClientFixture::registrations.empty() &&
          DeveloperDarknessClientFixture::warmed == std::vector<uint32_t>({0x101, 0x102, 0x103}),
          "A shared client/server context must skip registration while still warming both gun models and later models");
    onlyChanged(before, true, false);

    const auto rejectClient = [&] {
        const auto result = applyDeveloperDarkness(ctx, base, player, true, true, fixture.client);
        check(!result.applied && before == snapshot() && !DeveloperDarknessFixture::grants &&
              !DeveloperDarknessClientFixture::grows && !DeveloperDarknessClientFixture::names &&
              DeveloperDarknessClientFixture::registrations.empty() && DeveloperDarknessClientFixture::warmed.empty(),
              "Invalid client/renderer/resource-array handles reached client service calls or progression writes");
        onlyChanged(before, false, false);
    };
    prepareClient(); before = snapshot();
    memory->write32(fixture.client, 0x82080F80); rejectClient();
    memory->write32(fixture.client, 0x82081BF0);
    memory->write32(fixture.client + 516, 0x20); rejectClient(); memory->write32(fixture.client + 516, 0);
    memory->write32(fixture.client + 536, 4); rejectClient(); memory->write32(fixture.client + 536, 3);
    memory->write32(fixture.client + 432, 0); rejectClient();
    memory->write32(fixture.client + 432, DeveloperDarknessClientFixture::clientContext + 1); rejectClient();
    memory->write32(fixture.client + 432, DeveloperDarknessClientFixture::clientContext);
    memory->write32(DeveloperDarknessClientFixture::clientContext, 0x82070784); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::clientContext, 0x82070780);
    memory->write32(DeveloperDarknessClientFixture::clientContext + 80, 0); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::clientContext + 80, DeveloperDarknessClientFixture::provider + 4); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::clientContext + 80, DeveloperDarknessClientFixture::provider);
    memory->write32(fixture.client + 600, 0); rejectClient();
    memory->write32(fixture.client + 600, DeveloperDarknessClientFixture::renderer);
    memory->write32(DeveloperDarknessClientFixture::engineTable + 48, 0); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::engineTable + 48, 0x827AAB08);
    memory->write32(DeveloperDarknessClientFixture::serverArray + 4, 32769); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::serverArray + 4, DeveloperDarknessClientFixture::sourceCount);
    memory->write32(DeveloperDarknessClientFixture::clientArray + 4, 32769); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::clientArray + 4, DeveloperDarknessClientFixture::firstModel);
    memory->write32(DeveloperDarknessClientFixture::clientArray + 24, 0xFFFFFFFC); rejectClient();
    memory->write32(DeveloperDarknessClientFixture::clientArray + 24, DeveloperDarknessClientFixture::clientData);
    prepare();

    PPC_STORE_U16(state + 7672, 300);
    PPC_STORE_U8(state + 8553, 120); PPC_STORE_U8(state + 7656, 130);
    memory->write32(state + 7076, 0x80000201);
    before = snapshot();
    check(applyDeveloperDarkness(ctx, base, player, false, true).applied && before == snapshot(),
          "A maximum-level grant must not lower existing hearts, bonus capacity or bonus energy");

    const auto rejected = [&](const DeveloperPlayerHandles& handles, uint8_t* candidateBase) {
        const auto grantsBefore = DeveloperDarknessFixture::grants;
        const auto result = applyDeveloperDarkness(ctx, candidateBase, handles, true, true);
        check(!result.applied && !result.status.empty() && before == snapshot() &&
              DeveloperDarknessFixture::grants == grantsBefore &&
              memory->read32(DeveloperDarknessFixture::resources + 24) == DeveloperDarknessFixture::resourceFlags,
              "Invalid/stale player or resource handles reached Darkness progression writes or item creation");
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
    memory->write32(fixture.server + 432, 0); rejected(player, base);
    memory->write32(fixture.server + 432, DeveloperDarknessFixture::resources + 1); rejected(player, base);
    memory->write32(fixture.server + 432, 0xFFFFFFFC); rejected(player, base);
    memory->write32(fixture.server + 432, DeveloperDarknessFixture::resources);
    memory->write32(DeveloperDarknessFixture::resources, 0x82070784); rejected(player, base);
    memory->write32(DeveloperDarknessFixture::resources, 0x82070780);
    auto noStack = ctx; noStack.r1.u64 = 0;
    check(!applyDeveloperDarkness(noStack, base, player, true, true).applied && before == snapshot(),
          "Missing guest stack must reject an inventory grant before any progression writes");
    DWORD previous = 0;
    check(VirtualProtect(base + DeveloperDarknessFixture::resources, 0x1000, PAGE_READONLY, &previous),
          "Darkness fixture resource-context read-only protection");
    rejected(player, base);
    DWORD ignored = 0;
    check(VirtualProtect(base + DeveloperDarknessFixture::resources, 0x1000, previous, &ignored),
          "Darkness fixture resource-context protection restoration");
    check(VirtualProtect(base + state, 0x3000, PAGE_READONLY, &previous), "Darkness fixture read-only protection");
    rejected(player, base);
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
    puts("Developer Darkness: original thresholds/save/name lookup, Ancient repair, client fixed-ID sync/type3/model warming/retry/idempotence, memory guards and one-shot request consumption passed.");
}
