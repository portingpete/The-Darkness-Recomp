#pragma once

// Include after native_tests.cpp's check(). Call testFovCamera(ctx) after
// Memory::load and PPC dispatch initialization, serially with other contracts.
// Uses private guest fixtures and the ORIGINAL 8249A030 query / 821570E8 player
// FOV handler. Only the host dispatch-table entry for object-message routing is
// temporarily replaced; no game asset files or original image bytes are edited.
// Requires regenerated AOT hooks and runtime/native/fov_camera.cpp in DarkRuntime.
#include "runtime/native/fov_settings.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cmath>
#include <cstring>

void InitializeGameplayFovBaselineMidAsmHook(PPCRegister& state);
void ConfigureGameplayFovBaselineMidAsmHook(PPCRegister& state, PPCRegister& value);
void ForgetGameplayFovBaselineMidAsmHook(PPCRegister& component);
void CopyGameplayFovBaselineMidAsmHook(PPCRegister& destination, PPCRegister& source);
void ApplyGameplayFovMidAsmHook(PPCRegister& state, PPCRegister& viewport,
    PPCRegister& client, PPCRegister& message, PPCRegister& fov);

namespace FovCameraTestDetail {
inline thread_local uint32_t player = 0;

static PPC_FUNC(routeMessage) {
    // Real 824A6180 normally resolves the supplied object ID before making
    // exactly this player/client/message call. The camera query, original FOV
    // branches, FOV store, dirty flag and cache copy remain original PPC.
    const uint32_t client = ctx.r3.u32;
    const uint32_t message = ctx.r4.u32;
    ctx.r3.u64 = player;
    ctx.r4.u64 = client;
    ctx.r5.u64 = message;
    sub_821182A0(ctx, base); // Original P6 -> base-player -> message-36 handler.
}
} // namespace FovCameraTestDetail

static void testFovCamera(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    auto* base = memory->base();
    constexpr uint32_t dispatcher = 0x824A6180;
    constexpr uint32_t modVtable = 0x820807E0;
    constexpr uint32_t coreVtable = 0x8207F438;
    const uint32_t block = memory->allocate(0x40000);
    check(block != 0, "FOV camera fixture allocation");
    const uint32_t client = block, player = block + 0x4000;
    const uint32_t state = block + 0x6000, copyState = block + 0xA000;
    const uint32_t unknownState = block + 0xE000, viewport = block + 0x12000;
    const uint32_t message = block + 0x12400;
    struct Restore {
        uint8_t* base;
        uint32_t block;
        PPCFunc* dispatch;
        uint32_t previousPlayer;
        float setting;
        ~Restore() {
            PPC_LOOKUP_FUNC(base, dispatcher) = dispatch;
            FovCameraTestDetail::player = previousPlayer;
            setFieldOfViewSetting(setting);
            for (uint32_t offset : {0x6000u, 0xA000u, 0xE000u}) {
                PPCRegister component{};
                component.u64 = block + offset + 7072;
                ForgetGameplayFovBaselineMidAsmHook(component);
            }
            memory->release(block);
        }
    } restore{base, block, PPC_LOOKUP_FUNC(base, dispatcher),
              FovCameraTestDetail::player, fieldOfViewSetting()};
    std::memset(base + block, 0, 0x40000);
    check(memory->read32(modVtable + 436) == 0x8249A030 &&
          memory->read32(modVtable + 332) == dispatcher,
          "FOV fixture requires original Mod camera vtable");
    PPC_LOOKUP_FUNC(base, dispatcher) = FovCameraTestDetail::routeMessage;
    FovCameraTestDetail::player = player;
    auto putFloat = [&](uint32_t address, float value) {
        memory->write32(address, std::bit_cast<uint32_t>(value));
    };
    auto getFloat = [&](uint32_t address) {
        return std::bit_cast<float>(memory->read32(address));
    };
    auto freshContext = [&] {
        auto call = ctx;
        call.r1.u64 = block + 0x3F000;
        return call;
    };
    auto prepareState = [&](uint32_t address, float baseFov) {
        putFloat(address + 7160, baseFov);
        putFloat(address + 7152, baseFov);
        memory->write32(player + 396, address);
        // 821990D8 sees mode 1 and returns false, selecting the ordinary base
        // FOV branch. The independent forced-FOV branch is enabled below.
        memory->write32(player + 420, 0x100);
    };
    auto prepareViewport = [&] {
        auto call = freshContext();
        call.r3.u64 = viewport;
        sub_8275EE08(call, base);
        memory->write32(viewport + 324, 1720);
        memory->write32(viewport + 328, 720);
    };
    auto query = [&] {
        auto call = freshContext();
        call.r3.u64 = client;
        call.r4.u64 = viewport;
        sub_8249A030(call, base);
        check(base[viewport + 336] == 1, "FOV original store must dirty projection/culling");
        check(memory->read32(viewport + 260) == memory->read32(client + 2112 + 260),
              "FOV query must cache exactly the adjusted viewport");
        return getFloat(viewport + 260);
    };
    auto closeEnough = [](double a, double b) { return std::abs(a - b) < 0.00003; };
    constexpr double halfRadians = 3.14159265358979323846 / 360.0;

    memory->write32(client, modVtable);
    memory->write32(client + 536, 1);
    memory->write32(client + 540, 1);
    memory->write32(client + 8976, 0xFFFFFFFF);
    prepareState(state, 70);
    PPCRegister stateReg{};
    stateReg.u64 = state;
    InitializeGameplayFovBaselineMidAsmHook(stateReg);
    prepareViewport();
    setFieldOfViewSetting(0);
    check(query() == 70, "Original must retain original player base FOV");

    setFieldOfViewSetting(120);
    const float adjustedBase = query();
    check(closeEnough(guestFovToHorizontal16By9(adjustedBase, getFloat(viewport + 264)), 120),
          "Scoped active camera must implement configured horizontal 16:9 degrees");
    check(query() == adjustedBase, "Repeated camera queries must not compound FOV adjustment");
    {
        auto projection = freshContext();
        projection.r3.u64 = viewport;
        sub_8275EEF8(projection, base);
        check(base[viewport + 336] == 0 && getFloat(viewport + 260) == adjustedBase &&
              std::isfinite(getFloat(viewport)),
              "Original shared projection rebuild must consume the adjusted FOV");
    }

    // 82157448..8215746C: bit 27 in +7468 selects positive +7480 as final FOV.
    memory->write32(state + 7468, 0x08000000);
    putFloat(state + 7480, 35);
    const float adjustedZoom = query();
    check(closeEnough(std::tan(adjustedZoom * halfRadians) / std::tan(adjustedBase * halfRadians),
                      std::tan(35 * halfRadians) / std::tan(70 * halfRadians)),
          "Scoped original zoom must preserve its tangent ratio");
    setFieldOfViewSetting(0);
    check(query() == 35, "Switching back to Original must retain active zoom");

    // A scripted target eventually changes live +7160. It must not replace
    // the initialization/property baseline captured by the host.
    memory->write32(state + 7468, 0);
    putFloat(state + 7160, 50);
    setFieldOfViewSetting(120);
    const float scripted = query();
    check(closeEnough(std::tan(scripted * halfRadians) / std::tan(adjustedBase * halfRadians),
                      std::tan(50 * halfRadians) / std::tan(70 * halfRadians)),
          "Scripted live-base changes must not be normalized away");

    // Copy the effect-modified live state; transfer the original profile,
    // rather than treating copied live FOV=50 as a new unzoomed baseline.
    std::memcpy(base + copyState, base + state, 0x4000);
    PPCRegister destination{}, source{};
    destination.u64 = copyState + 7072;
    source.u64 = state + 7072;
    CopyGameplayFovBaselineMidAsmHook(destination, source);
    memory->write32(player + 396, copyState);
    check(query() == scripted, "State copies must retain the source FOV profile");

    prepareState(state, 80);
    PPCRegister propertyValue{};
    propertyValue.f64 = 80;
    ConfigureGameplayFovBaselineMidAsmHook(stateReg, propertyValue);
    const float propertyBase = query();
    check(closeEnough(guestFovToHorizontal16By9(propertyBase, getFloat(viewport + 264)), 120),
          "Asset/property profile must define the configured unzoomed FOV");
    putFloat(state + 7160, 40);
    check(closeEnough(std::tan(query() * halfRadians) / std::tan(propertyBase * halfRadians),
                      std::tan(40 * halfRadians) / std::tan(80 * halfRadians)),
          "Property baseline must survive later live-base effects");

    prepareState(unknownState, 70); // deliberately no initialization hook
    PPCRegister unknownComponent{};
    unknownComponent.u64 = unknownState + 7072;
    ForgetGameplayFovBaselineMidAsmHook(unknownComponent);
    check(query() == 70, "Unknown baseline must not alter a camera");
    prepareState(state, 70);
    InitializeGameplayFovBaselineMidAsmHook(stateReg);
    memory->write32(client, 0x82081BF0); // Original P6 client vtable.
    check(closeEnough(query(), adjustedBase), "P6 client must use the same scoped camera setting");
    memory->write32(client, modVtable);
    prepareViewport();
    putFloat(viewport + 264, 2);
    check(query() == 70, "Unsupported projection convention must remain original");
    prepareViewport();
    memory->write32(client, coreVtable);
    check(query() == 70, "Non-gameplay client query must remain original");
    memory->write32(client, modVtable);
    memory->write32(client + 8976, 0);
    check(query() == 70, "Authored square-camera query must remain original");
    memory->write32(client + 8976, 0xFFFFFFFF);

    // Direct calls outside the camera-query wrapper must never acquire scope.
    PPCRegister vpReg{}, clientReg{}, messageReg{}, angle{};
    vpReg.u64 = viewport; clientReg.u64 = client; messageReg.u64 = message;
    angle.f64 = 35;
    memory->write32(message, 36);
    memory->write32(message + 40, viewport);
    memory->write32(message + 44, 416);
    ApplyGameplayFovMidAsmHook(stateReg, vpReg, clientReg, messageReg, angle);
    check(angle.f64 == 35, "Unscoped player/message call must remain original");
    std::puts("Scoped original player camera FOV contract passed.");
}
