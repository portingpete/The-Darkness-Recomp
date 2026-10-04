#include "developer_darkness.h"
#include "developer_player_lookup.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>

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
}

DeveloperDarknessResult applyDeveloperDarkness(PPCContext& ctx, uint8_t* base,
    const DeveloperPlayerHandles& player, bool unlock, bool maximum) {
    if (!unlock && !maximum) return {};
    if (!currentPlayer(base, player))
        return {false, "Darkness grants require an active player."};
    const auto state = player.state;
    uint32_t dirty = PPC_LOAD_U32(state + kDirty);
    const auto putByte = [&](uint32_t offset, uint8_t value, uint32_t flag) {
        if (PPC_LOAD_U8(state + offset) == value) return;
        PPC_STORE_U8(state + offset, value);
        dirty |= flag;
    };
    if (unlock) {
        // The original giveall branch grants exactly these six bits. Preserve
        // each field's unrelated high bits instead of granting its weapons,
        // Darkling variants, collectibles or profile achievements as well.
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
    if (unlock && maximum) return {true, "All Darkness powers unlocked; Darkness level maximized. Grants may be autosaved."};
    if (unlock) return {true, "All Darkness powers unlocked. Grants may be autosaved."};
    return {true, "Darkness level maximized. Grants may be autosaved."};
}
}
