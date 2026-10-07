#include "developer_darkness.h"
#include "developer_player_lookup.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>

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

bool grantAncientWeapons(PPCContext& ctx, uint8_t* base, const DeveloperPlayerHandles& player) {
    uint32_t inventory;
    if (!inventoryReady(ctx, base, player, inventory)) return false;
    for (const auto name : kAncientNames) {
        if (findItem(ctx, base, inventory, name)) continue;
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
        if (!currentPlayer(base, player) || !inventoryReady(ctx, base, player, inventory) ||
            !findItem(ctx, base, inventory, name)) return false;
    }
    return true;
}
}

DeveloperDarknessResult applyDeveloperDarkness(PPCContext& ctx, uint8_t* base,
    const DeveloperPlayerHandles& player, bool unlock, bool maximum) {
    if (!unlock && !maximum) return {};
    if (!currentPlayer(base, player))
        return {false, "Darkness grants require an active player."};
    const auto state = player.state;
    if (unlock && !grantAncientWeapons(ctx, base, player))
        return {false, "Darkness gun grant incomplete. Retry with an active player inventory. Grants may be autosaved."};
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
