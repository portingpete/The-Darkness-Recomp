#pragma once
#include "runtime/native/movement.h"
#include "runtime/native/movement_settings.h"
#include <array>
#include <bit>
#include <cmath>
#include <vector>

static void testMovementCameraRecovery(PPCContext&) {
    using namespace DarkRecomp::Native;
    const auto block = memory->allocate(0x10000);
    check(block != 0, "Camera recovery fixture allocation");
    const auto oldStyle = movementStyle();
    struct Release {
        uint32_t block; MovementStyle style;
        ~Release() { setMovementStyle(style); memory->release(block); }
    } release{block, oldStyle};
    auto* base = memory->base();
    std::memset(base + block, 0, 0x10000);
    const auto client = block, actor = block + 0x1000, state = block + 0x2000;
    const auto history = block + 0x6000;
    memory->write32(client + 536, 3); PPC_STORE_U16(actor + 368, 3);
    memory->write32(actor + 396, state);
    memory->write32(state + 16, actor); memory->write32(state + 56, client);
    memory->write32(state + 32, 100); memory->write32(history + 32, 100);
    const auto putFloat = [&](uint32_t address, float value) { memory->write32(address, std::bit_cast<uint32_t>(value)); };
    putFloat(history + 1876, 31); putFloat(history + 1880, 0);
    PPC_STORE_U64(history + 1896, std::bit_cast<uint64_t>(10.0));
    // Verify that both hooks still sit on the original retail instructions.
    check(memory->read32(0x821DC110) == 0x397C0754 && memory->read32(0x821DE4BC) == 0xC9B20768,
          "Camera recovery hook instruction proof");
    PPCRegister cl{}, ar{}, st{}, hs{}, reset{}, limit{}, distance{}, dt{}, time{}, grace{};
    cl.u64 = client; ar.u64 = actor; st.u64 = state; hs.u64 = history;
    distance.f64 = 31; dt.f64 = 1.0 / 360; time.f64 = 10.49; grace.f64 = 0.5;
    const auto invoke = [&] {
        limit.f64 = 4;
        const bool recovered = RecoverMovementCameraLagMidAsmHook(cl, ar, st, hs, reset, limit, distance, dt, time, grace);
        check(recovered || limit.f64 == 4, "Unselected clamp branch must retain the original radius");
        return float(limit.f64);
    };
    check(invoke() == 4, "Camera recovery cannot use history outside its invocation");
    const auto* disabled = std::getenv("DARK_MOVEMENT_CAMERA_RECOVERY");
    const bool enabled = !(disabled && disabled[0] == '0' && !disabled[1]);
    for (auto style : {MovementStyle::Original, MovementStyle::Alternate}) {
        setMovementStyle(style);
        putFloat(history + 1876, 31);
        const std::vector<uint8_t> before(base + block, base + block + 0x10000);
        {
            MovementCameraHistoryScope scope(base, client, actor);
            CaptureMovementCameraHistoryMidAsmHook(hs);
            check(std::memcmp(before.data(), base + block, before.size()) == 0,
                  "History capture cannot change guest state");
            // Both DC078 and its DD7F8 child clear this correction. Reading it
            // for the first time inside the child produced zero in every live
            // build13 row, turning intended recovery into an immediate clamp.
            putFloat(history + 1876, 0); putFloat(history + 1880, 0); putFloat(history + 1884, 0);
            CaptureMovementCameraHistoryMidAsmHook(hs); // A repeated inner capture must not replace it.
            check(scope.previousLag == 31, "Complete camera scope must retain history across both retail clears");
            const std::vector<uint8_t> afterRetailClears(base + block, base + block + 0x10000);
            const auto recovered = invoke();
            check(enabled ? recovered > 30 && recovered < 31 : recovered == 4,
                  "Both movement styles must use continuous collision-lag recovery");
            check(std::memcmp(afterRetailClears.data(), base + block, afterRetailClears.size()) == 0,
                  "Recovery changes only the original distance-limit register, not body, clock or guest history");
            distance.f64 = 10;
            check(invoke() == (enabled ? 10 : 4), "Recovery must not lengthen an already smaller camera offset");
            check(distance.f64 == 10, "Recovery must preserve the original offset-distance register");
            distance.f64 = 31;
            reset.u64 = 1; check(invoke() == 4, "Camera cut must keep its immediate reset"); reset.u64 = 0;
            time.f64 = 10.51; check(invoke() == 4, "Expired collision grace must keep its original cap"); time.f64 = 10.49;
            memory->write32(state + 9752, 99);
            check(invoke() == 4, "Recent forced pose must keep retail behavior"); memory->write32(state + 9752, 0);
            memory->write32(history + 9752, 99);
            check(invoke() == 4, "Recent shared-history transform must keep retail behavior"); memory->write32(history + 9752, 0);
            for (const auto flags : {0x4000u, 0x100u}) {
                memory->write32(actor + 420, flags);
                check(invoke() == 4, "Nonordinary movement or posture cannot enter camera recovery");
            }
            memory->write32(actor + 420, 0);
            for (const auto flags : {0x20000000u, 0x4000000u, 0x100000u}) {
                memory->write32(actor + 364, flags); check(invoke() == 4, "Special actor camera keeps retail behavior");
            }
            memory->write32(actor + 364, 0);
            PPC_STORE_U16(state + 8300, 1); check(invoke() == 4, "Scripted target bypasses recovery"); PPC_STORE_U16(state + 8300, 0);
            for (const auto flags : {2u, 8u, 16u}) {
                PPC_STORE_U8(state + 10300, flags); check(invoke() == 4, "Special camera flags bypass recovery");
            }
            PPC_STORE_U8(state + 10300, 0);
            memory->write32(state + 392, 0x100000);
            check(invoke() == 4, "Forced camera state bypasses recovery"); memory->write32(state + 392, 0);
            memory->write32(state + 10240, 0x400000);
            check(invoke() == 4, "Alternate camera branch bypasses recovery"); memory->write32(state + 10240, 0);
            {
                MovementCameraHistoryScope nested(base, client, actor + 0x100);
                CaptureMovementCameraHistoryMidAsmHook(hs);
                check(invoke() == 4, "Nested unrelated camera cannot reuse the player's history");
            }
            check(enabled ? invoke() > 30 : invoke() == 4, "Nested camera must restore the outer scope");
            dt.f64 = -0.01; check(invoke() == 4, "Invalid camera clock keeps retail reset"); dt.f64 = 1.0 / 360;
        }
        check(invoke() == 4, "Camera history scope must not leak to subsequent calls");
    }
    std::fprintf(stderr, "[MovementCameraContract] originalLimit=4 previousOffset=31 candidateLimit=%.9g enabled=%u; both styles, resets, nesting and guest-state preservation passed\n",
        enabled ? 4 + 27 * std::exp(-12.f / 360) : 4.f, unsigned(enabled));
}
