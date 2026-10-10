#include "developer_darkness.h"
#include "developer_player_lookup.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <map>

namespace DarkRecomp::Native {
namespace {
// Retail82126828's giveall/level commands and the named power-bit table at
// 82A473AC establish these fields. The six bits are CreepingDark, DemonArm,
// AncientWeapons, BlackHole, DarknessVision and DarknessShield respectively.
constexpr uint32_t kDirty = 7076;
constexpr uint32_t kEnergy = 7656;
constexpr uint32_t kUsablePowers = 7659;
constexpr uint32_t kUnlockedPowers = 7660;
constexpr uint32_t kProgress = 7668;
constexpr uint32_t kHearts = kProgress + 4;
constexpr uint32_t kCapacity = 8553;
constexpr uint8_t kAllPowers = 0x3F;
constexpr uint16_t kMaximumHearts = 180;
constexpr uint8_t kMaximumEnergy = 100;
constexpr uint32_t kGrantItem = 0x8218B4C0;
constexpr std::array<uint32_t, 2> kAncientNames{0x82062D00, 0x82062D14};

bool mapped(uint8_t* base, uint32_t address, size_t bytes, bool writable) {
    if (!memory || base != memory->base() || !address || !bytes ||
        uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    auto* cursor = base + address;
    const auto* end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = info.Protect & 0xFF;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool canRead = canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if (!canRead || (writable && !canWrite)) return false;
        cursor = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

bool currentPlayer(uint8_t* base, const DeveloperPlayerHandles& player) {
    if ((player.actor | player.state | player.server) & 3) return false;
    if (!mapped(base, player.actor, 640, false) ||
        !mapped(base, player.server, 4, false) ||
        !mapped(base, player.state, kCapacity + 1, true)) return false;
    const auto table = PPC_LOAD_U32(player.server);
    return (table == 0x82072370 || table == 0x82080F80) &&
        PPC_LOAD_U32(player.actor + 396) == player.state &&
        PPC_LOAD_U32(player.actor + 636) == player.server &&
        PPC_LOAD_U16(player.actor + 368) && PPC_LOAD_U16(player.actor + 370);
}

bool object(uint8_t* base, uint32_t address, size_t bytes, bool writable = false) {
    return !(address & 3) && mapped(base, address, bytes, writable);
}

uint32_t method(uint8_t* base, uint32_t address, uint32_t offset) {
    if (!object(base, address, 4)) return 0;
    const auto table = PPC_LOAD_U32(address);
    if (!object(base, table, size_t(offset) + 4)) return 0;
    const auto function = PPC_LOAD_U32(table + offset);
    return !(function & 3) && function >= PPC_CODE_BASE &&
        uint64_t(function) < uint64_t(PPC_CODE_BASE) + PPC_CODE_SIZE &&
        PPC_LOOKUP_FUNC(base, function) ? function : 0;
}

bool array(uint8_t* base, uint32_t address, uint32_t limit, uint32_t& data, uint32_t& count) {
    data = count = 0;
    if (!address) return true; // An empty original TArray has no allocation.
    if (!object(base, address, 28, true)) return false;
    count = PPC_LOAD_U32(address + 4);
    if (count > limit) return false;
    data = PPC_LOAD_U32(address + 24);
    return !count || object(base, data, size_t(count) * 4, true);
}

bool itemName(uint8_t* base, uint32_t item) {
    // Original name lookup calls the concrete CStr's narrow/wide accessors.
    // Check that object and its backing before handing traversal to the game.
    const auto table = PPC_LOAD_U32(item + 8);
    if (!object(base, table, 80) || PPC_LOAD_U32(table + 76) != 0x821F7A38 ||
        PPC_LOAD_U32(table + 28) != 0x821F76B8 || PPC_LOAD_U32(table + 48) != 0x821F7760) return false;
    const auto backing = PPC_LOAD_U32(item + 12);
    if (!backing) return true;
    if (!mapped(base, backing, 2, false)) return false;
    const uint32_t width = (PPC_LOAD_U16(backing) & 0x8000) ? 2 : 1;
    for (uint32_t i = 0; i < 512; ++i) {
        const uint64_t address = uint64_t(backing) + 2 + i * width;
        if (address > UINT32_MAX || !mapped(base, uint32_t(address), width, false)) return false;
        if (width == 1 ? !PPC_LOAD_U8(uint32_t(address)) : !PPC_LOAD_U16(uint32_t(address))) return true;
    }
    return false;
}

bool inventoryReady(PPCContext& ctx, uint8_t* base, const DeveloperPlayerHandles& player, uint32_t& inventory) {
    if (!object(base, player.actor, 1656, true) || !object(base, player.state, 10388, true) ||
        !object(base, player.server, 1464) || ctx.r1.u32 < 0x4000 ||
        !mapped(base, ctx.r1.u32 - 0x4000, 0x4000, true)) return false;
    inventory = PPC_LOAD_U32(player.actor + 656);
    if (!object(base, inventory, 88, true) || !object(base, PPC_LOAD_U32(inventory + 16), 4)) return false;
    uint32_t categories, categoryCount;
    if (!array(base, PPC_LOAD_U32(inventory + 24), 16, categories, categoryCount) || categoryCount < 4) return false;
    for (uint32_t c = 0; c < categoryCount; ++c) {
        const auto category = PPC_LOAD_U32(categories + c * 4);
        if (!object(base, category, 88, true)) return false;
        uint32_t items, itemCount;
        if (!array(base, PPC_LOAD_U32(category + 24), 4096, items, itemCount)) return false;
        for (uint32_t i = 0; i < itemCount; ++i) {
            const auto item = PPC_LOAD_U32(items + i * 4);
            if (!object(base, item, 720, true) || !itemName(base, item)) return false;
        }
    }
    return true;
}

uint32_t findItem(PPCContext& ctx, uint8_t* base, uint32_t inventory, uint32_t name) {
    auto call = ctx;
    call.r3.u64 = inventory; call.r4.u64 = name;
    sub_823241C0(call, base);
    return call.r3.u32;
}

bool clientAncientResources(PPCContext& ctx, uint8_t* base, const DeveloperPlayerHandles& player,
    uint32_t client, uint32_t resources, const std::array<uint16_t, 2>& models) {
    if (!client) return true; // Direct progression callers may have no renderer.
    if (!object(base, client, 604) ||
        (PPC_LOAD_U32(client) != 0x820807E0 && PPC_LOAD_U32(client) != 0x82081BF0) ||
        (PPC_LOAD_U32(client + 516) & 0x20) ||
        PPC_LOAD_U32(client + 536) != PPC_LOAD_U16(player.actor + 368)) return false;
    const auto target = PPC_LOAD_U32(client + 432);
    const auto renderer = PPC_LOAD_U32(client + 600);
    if (!object(base, target, 88, true) || PPC_LOAD_U32(target) != 0x82070780 ||
        !PPC_LOAD_U32(resources + 80) || PPC_LOAD_U32(target + 80) != PPC_LOAD_U32(resources + 80) ||
        !object(base, renderer, 376) || !method(base, renderer, 48) || !method(base, renderer, 52)) return false;
    uint32_t sourceData, sourceCount, targetData, targetCount;
    if (!array(base, PPC_LOAD_U32(resources + 12), 32768, sourceData, sourceCount) ||
        !array(base, PPC_LOAD_U32(target + 12), 32768, targetData, targetCount) ||
        !models[0] || !models[1] || models[0] >= sourceCount || models[1] >= sourceCount) return false;
    if (!PPC_LOAD_U32(sourceData + models[0] * 4) || !PPC_LOAD_U32(sourceData + models[1] * 4)) return false;
    struct PendingPrecache {
        uint32_t source = 0, target = 0, provider = 0;
        std::map<uint32_t, uint32_t> additions;
    };
    static thread_local PendingPrecache pending;
    const auto provider = PPC_LOAD_U32(resources + 80);
    if (pending.source != resources || pending.target != target || pending.provider != provider)
        pending = {resources, target, provider, {}};
    const auto first = (std::min)({targetCount, uint32_t(models[0]), uint32_t(models[1])});
    std::fprintf(stderr, "[Weapons] Preparing client resources: server=%08x client=%08x counts=%u/%u models=%u/%u renderer=%08x.\n",
        resources, target, sourceCount, targetCount, models[0], models[1], renderer);
    // Retail's fixed-ID client receiver (824A7C48) runs only during world
    // loading. Late server additions do not reach its frozen client table.
    // Preserve the existing SmartRefs and append empty slots through the same
    // TArray helper used by ordinary resource registration.
    if (target != resources && targetCount < sourceCount) {
        auto call = ctx; call.r3.u64 = target + 8; call.r4.u64 = sourceCount;
        PPCSafeIndirect(call, base, 0x82346CE0);
        if (!array(base, PPC_LOAD_U32(target + 12), 32768, targetData, targetCount) ||
            targetCount < sourceCount) return false;
    }
    auto frame = ctx;
    frame.r1.u32 -= 0x100;
    PPC_STORE_U32(frame.r1.u32, ctx.r1.u32);
    const auto name = frame.r1.u32 + 80;
    for (uint32_t i = 0; i < sourceCount; ++i) {
        const auto resource = PPC_LOAD_U32(sourceData + i * 4);
        if (!resource) continue;
        if (!object(base, resource, 36, true)) return false;
        if (PPC_LOAD_U32(resource + 16) & 0x10000000) continue;
        const auto existing = PPC_LOAD_U32(targetData + i * 4);
        // Both contexts use the same provider, whose normalized name lookup
        // returns the already retained global resource. Never replace a
        // conflicting client ID or copy a borrowed pointer without retaining it.
        if (existing && i >= first && existing != resource) return false;
        if (!existing) {
            PPC_STORE_U32(name, 0x82065568); PPC_STORE_U32(name + 4, 0);
            struct ReleaseName {
                PPCContext frame;
                uint8_t* base;
                uint32_t name;
                ~ReleaseName() { frame.r3.u64 = name; sub_821F8AD0(frame, base); }
            } releaseName{frame, base, name};
            auto call = frame;
            call.r3.u64 = name; call.r4.u64 = resources; call.r5.u64 = i;
            PPCSafeIndirect(call, base, 0x82342548);
            const auto narrow = method(base, name, 20);
            if (!narrow) return false;
            call = frame; call.r3.u64 = name;
            PPCSafeIndirect(call, base, narrow);
            const auto text = call.r3.u32;
            bool terminated = false;
            for (uint32_t n = 0; text && n < 2048; ++n) {
                if (uint64_t(text) + n > UINT32_MAX || !mapped(base, text + n, 1, false)) return false;
                if (!PPC_LOAD_U8(text + n)) { terminated = n != 0; break; }
            }
            if (!terminated) return false;
            call = frame; call.r3.u64 = target; call.r4.u64 = i;
            call.r5.u64 = text; call.r6.s64 = int8_t(PPC_LOAD_U8(resource + 17));
            PPCSafeIndirect(call, base, 0x82342600);
            if (!array(base, PPC_LOAD_U32(target + 12), 32768, targetData, targetCount) ||
                i >= targetCount || PPC_LOAD_U32(targetData + i * 4) != resource) return false;
            pending.additions[i] = resource;
        }
    }
    // Metadata loading alone does not populate the mesh's vertex-buffer IDs,
    // primitive cache or material images. Use CWRes_Model's original virtual
    // precache, with the same CXR engine supplied by client preload 8249E448.
    const auto modelToken = PPC_LOAD_U32(0x82A45900);
    std::array<bool, 2> prepared{};
    for (uint32_t i = 0; i < sourceCount; ++i) {
        const auto resource = PPC_LOAD_U32(targetData + i * 4);
        // An interrupted registration may already have retained an earlier
        // projectile dependency without reaching the warm pass. Remember those
        // IDs until completion; revalidate both tables before using them.
        if (i < first) {
            const auto added = pending.additions.find(i);
            if (added == pending.additions.end() || added->second != resource ||
                PPC_LOAD_U32(sourceData + i * 4) != resource) continue;
        }
        if (!resource) continue;
        if (!object(base, resource, 36)) return false;
        if (PPC_LOAD_U32(resource + 16) & 0x10000000) continue;
        const auto type = method(base, resource, 0);
        if (!type) return false;
        auto call = frame; call.r3.u64 = resource;
        PPCSafeIndirect(call, base, type);
        auto descriptor = call.r3.u32;
        for (unsigned depth = 0; descriptor && depth < 64; ++depth) {
            if (!object(base, descriptor, 12)) return false;
            if (PPC_LOAD_U32(descriptor) == modelToken) {
                const auto precache = method(base, resource, 68);
                if (!precache) return false;
                call = frame; call.r3.u64 = resource; call.r4.u64 = renderer;
                PPCSafeIndirect(call, base, precache);
                if (i == models[0]) prepared[0] = true;
                if (i == models[1]) prepared[1] = true;
                break;
            }
            descriptor = PPC_LOAD_U32(descriptor + 8);
        }
    }
    const bool complete = prepared[0] && prepared[1] &&
        PPC_LOAD_U32(targetData + models[0] * 4) == PPC_LOAD_U32(sourceData + models[0] * 4) &&
        PPC_LOAD_U32(targetData + models[1] * 4) == PPC_LOAD_U32(sourceData + models[1] * 4);
    if (complete) pending.additions.clear();
    return complete;
}

bool repairAncientResources(PPCContext& ctx, uint8_t* base, const DeveloperPlayerHandles& player,
    uint32_t resources, uint32_t item, uint32_t name) {
    if (!object(base, item, 960, true) || PPC_LOAD_U32(item) != 0x8206C5B0 ||
        PPC_LOAD_U32(item + 16) != player.server) return false;
    const auto ready = [&] { return PPC_LOAD_U16(item + 466) && int16_t(PPC_LOAD_U16(item + 636)) > 0; };
    if (ready() && PPC_LOAD_U32(item + 340) && PPC_LOAD_U32(item + 344)) return true;
    const bool repairEffects = !ready();

    // A previous late grant may already be in the inventory. Reuse the retail
    // template lookup/precache/key parser (82332D48), replaying only missing
    // resources so weapon IDs, ammunition, flags and equipped clones survive.
    auto frame = ctx;
    frame.r1.u32 -= 0x100;
    PPC_STORE_U32(frame.r1.u32, ctx.r1.u32);
    const auto reference = frame.r1.u32 + 80;
    PPC_STORE_U32(reference, 0);
    auto call = frame;
    call.r3.u64 = reference; call.r4.u64 = name; call.r5.u64 = player.server;
    PPCSafeIndirect(call, base, 0x823328E8);
    struct ReleaseTemplate {
        PPCContext frame;
        uint8_t* base;
        uint32_t reference;
        ~ReleaseTemplate() {
            frame.r3.u64 = reference;
            PPCSafeIndirect(frame, base, 0x820CBBC8);
        }
    } releaseTemplate{frame, base, reference};
    const auto registry = PPC_LOAD_U32(reference);
    if (!object(base, registry, 4)) return false;
    const auto table = PPC_LOAD_U32(registry);
    if (!object(base, table, 68)) return false;
    call = frame;
    call.r3.u64 = item; call.r4.u64 = registry; call.r5.u64 = resources; call.r6.u64 = player.server;
    PPCSafeIndirect(call, base, PPC_LOAD_U32(0x8206C5B0 + 32));
    call = frame; call.r3.u64 = registry;
    PPCSafeIndirect(call, base, PPC_LOAD_U32(table + 52));
    const auto count = call.r3.u32;
    if (count > 4096) return false;
    for (uint32_t i = 0; i < count; ++i) {
        call = frame; call.r3.u64 = registry; call.r4.u64 = i;
        PPCSafeIndirect(call, base, PPC_LOAD_U32(table + 64));
        const auto key = call.r3.u32;
        if (!object(base, key, 4) || !object(base, PPC_LOAD_U32(key), 432)) return false;
        call = frame; call.r3.u64 = key;
        PPCSafeIndirect(call, base, PPC_LOAD_U32(PPC_LOAD_U32(key) + 428));
        const auto hash = call.r3.u32;
        const bool missing = ((hash == 0x0FF1EEF1 || hash == 0x0E326DE1) && !PPC_LOAD_U16(item + 466)) ||
            (hash == 0x713CE1A9 && int16_t(PPC_LOAD_U16(item + 636)) <= 0) ||
            (hash == 0x86314D73 && !PPC_LOAD_U32(item + 340)) ||
            ((hash == 0x1562BFDF || hash == 0xC5F42FDC) && !PPC_LOAD_U32(item + 344)) ||
            (hash == 0xA7C1B409 && repairEffects);
        if (!missing) continue;
        call = frame; call.r3.u64 = item; call.r4.u64 = hash; call.r5.u64 = key;
        PPCSafeIndirect(call, base, PPC_LOAD_U32(0x8206C5B0 + 44));
    }
    return ready() && PPC_LOAD_U32(item + 340) && PPC_LOAD_U32(item + 344);
}

bool grantAncientWeapons(PPCContext& ctx, uint8_t* base, const DeveloperPlayerHandles& player,
    uint32_t client, std::string& failure) {
    uint32_t inventory;
    if (!inventoryReady(ctx, base, player, inventory)) return false;
    const auto resources = PPC_LOAD_U32(player.server + 432);
    if (!object(base, resources, 84, true) || PPC_LOAD_U32(resources) != 0x82070780) return false;
    // Early maps do not cache the late-game guns. Supply their owned cached
    // files before asking the original resource provider to load the template.
    // Activation lasts for this runtime so deferred model reads still work.
    try {
        memory->enableDeveloperWeaponAssets(ctx, base);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[Weapons] Darkness grant assets unavailable: %s.\n", error.what());
        failure = "Darkness gun grant incomplete: " + std::string(error.what());
        if (!failure.ends_with('.')) failure += '.';
        failure += " Grants may be autosaved.";
        return false;
    }
    // World loading freezes the resource registry. Retail dynamic spawning
    // (822A6410, 822B19C8) temporarily permits additions while evaluating an
    // object's template, then restores the original flags. Without that scope,
    // a late Ancient grant silently stores zero model/animation/effect IDs.
    const auto resourceFlags = PPC_LOAD_U32(resources + 24);
    struct RestoreResources {
        uint8_t* base;
        uint32_t resources, flags;
        ~RestoreResources() { PPC_STORE_U32(resources + 24, flags); }
    } restoreResources{base, resources, resourceFlags};
    PPC_STORE_U32(resources + 24, resourceFlags & ~1u);
    const auto repairPair = [&](uint32_t item, uint32_t name) {
        if (!repairAncientResources(ctx, base, player, resources, item, name)) return false;
        const auto cloneId = PPC_LOAD_U32(item + 896);
        if (cloneId == UINT32_MAX) return true;
        // Equipped replicas have their own parsed resources. Resolve the
        // recorded ID in category2 through the retail lookup, without replacing
        // either item or changing the inventory's equip/identity bookkeeping.
        const auto categories = PPC_LOAD_U32(inventory + 24);
        const auto category = PPC_LOAD_U32(PPC_LOAD_U32(categories + 24) + 8);
        auto call = ctx; call.r3.u64 = category; call.r4.u64 = cloneId;
        sub_8232A800(call, base);
        const auto clone = call.r3.u32;
        return !clone || (PPC_LOAD_U32(clone + 320) == (7 | 0x200) &&
            repairAncientResources(ctx, base, player, resources, clone, name));
    };
    std::array<uint16_t, 2> models{};
    for (size_t i = 0; i < kAncientNames.size(); ++i) {
        const auto name = kAncientNames[i];
        if (const auto item = findItem(ctx, base, inventory, name)) {
            if (!repairPair(item, name)) return false;
            models[i] = PPC_LOAD_U16(item + 466);
            continue;
        }
        auto call = ctx;
        // Retail giveall821279E4 grants only these templates with this ABI.
        // Their authored unique/forceequipright/left rules create and equip
        // the pair through the engine; no story key or other item is granted.
        call.r3.u64 = player.actor; call.r4.u64 = 0; call.r5.u64 = 0;
        call.r6.u64 = name; call.r7.s64 = int16_t(PPC_LOAD_U16(player.actor + 368));
        call.r8.u64 = 0; call.r9.u64 = 0;
        PPCSafeIndirect(call, base, kGrantItem);
        // The helper's return value is not an inventory-success boolean.
        // Verify the actual original lookup, including its removed-item filter.
        if (!currentPlayer(base, player) || !inventoryReady(ctx, base, player, inventory)) return false;
        const auto item = findItem(ctx, base, inventory, name);
        if (!item || !repairPair(item, name)) return false;
        models[i] = PPC_LOAD_U16(item + 466);
    }
    if (!clientAncientResources(ctx, base, player, client, resources, models)) {
        failure = "Darkness gun client resources incomplete. Retry with an active player. Grants may be autosaved.";
        std::fprintf(stderr, "[Weapons] Client resource preparation failed: client=%08x serverContext=%08x.\n", client, resources);
        return false;
    }
    return true;
}
}

DeveloperDarknessResult applyDeveloperDarkness(PPCContext& ctx, uint8_t* base,
    const DeveloperPlayerHandles& player, bool unlock, bool maximum, uint32_t client) {
    if (!unlock && !maximum) return {};
    if (!currentPlayer(base, player))
        return {false, "Darkness grants require an active player."};
    const auto state = player.state;
    std::string failure;
    if (unlock && !grantAncientWeapons(ctx, base, player, client, failure))
        return {false, failure.empty() ?
            "Darkness gun grant incomplete. Retry with an active player inventory. Grants may be autosaved." : std::move(failure)};
    // Read after the original inventory/equip helper, which sets its own dirty
    // flags. Retaining a pre-call value here would erase those engine updates.
    uint32_t dirty = PPC_LOAD_U32(state + kDirty);
    const auto putByte = [&](uint32_t offset, uint8_t value, uint32_t flag) {
        if (PPC_LOAD_U8(state + offset) == value) return;
        PPC_STORE_U8(state + offset, value);
        dirty |= flag;
    };
    if (unlock) {
        // The original giveall branch grants exactly these six bits. Preserve
        // each field's unrelated high bits. The two Ancient templates above
        // accompany their power; Darklings, collectibles and achievements do not.
        putByte(kUnlockedPowers, PPC_LOAD_U8(state + kUnlockedPowers) | kAllPowers, 0x100);
        putByte(kUsablePowers, PPC_LOAD_U8(state + kUsablePowers) | kAllPowers, 0x100);
    }
    if (maximum) {
        if (PPC_LOAD_U16(state + kHearts) < kMaximumHearts) {
            // Original givedarknesslevel5 uses index4. This setter stores its
            // authored180-heart threshold; no fabricated level field exists.
            auto call = ctx;
            call.r3.u64 = state + kProgress;
            call.r4.u64 = 4;
            sub_821A8CF0(call, base);
            dirty |= 0x100;
        }
        // Original command updates max/current Darkness with dirty4000/100.
        // A grant must never lower existing bonus capacity, energy or hearts.
        const auto capacity = (std::max)(PPC_LOAD_U8(state + kCapacity), kMaximumEnergy);
        putByte(kCapacity, capacity, 0x4000);
        putByte(kEnergy, (std::max)(PPC_LOAD_U8(state + kEnergy), capacity), 0x100);
    }
    PPC_STORE_U32(state + kDirty, dirty);
    if (unlock && maximum) return {true, "All Darkness powers and guns unlocked; Darkness level maximized. Grants may be autosaved."};
    if (unlock) return {true, "All Darkness powers and guns unlocked. Grants may be autosaved."};
    return {true, "Darkness level maximized. Grants may be autosaved."};
}
}
