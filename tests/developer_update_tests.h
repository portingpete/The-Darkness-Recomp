#pragma once
#include "runtime/native/developer_missions.h"
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_820C5D30);
extern "C" PPC_FUNC(__imp__sub_820F68D8);

namespace DeveloperUpdateFixture {
constexpr uint32_t noOpAddress = 0x827AAB08;
constexpr uint32_t applicationStepAddress = 0x820FF0B0;
inline uint32_t application, manager;
inline unsigned originalCalls;
inline bool requireQueued;

static PPC_FUNC(originalStep) {
    ++originalCalls;
    check(originalCalls <= 2 && ctx.r3.u32 == (originalCalls == 1 ? manager : application),
          "original game update did not complete its manager and GUI steps in order");
    if (requireQueued)
        check(developerSnapshot().status == "Mission load queued.",
              "developer command ran before the entire original game update returned");
    // Make preservation observable beyond a no-op's unchanged registers.
    ctx.r8.u64 = 0x1234567800000000ull + originalCalls;
}
}

static void testDeveloperUpdate(PPCContext& ctx) {
    using namespace DeveloperUpdateFixture;
    auto* base = memory->base();
    const auto fixture = memory->allocate(0x8000);
    check(fixture != 0, "developer update fixture allocation failed");
    const auto oldSystem = memory->read32(0x82A690F8);
    const auto oldNoOp = PPC_LOOKUP_FUNC(base, noOpAddress);
    const auto oldApplicationStep = PPC_LOOKUP_FUNC(base, applicationStepAddress);
    struct Restore {
        uint8_t* base; uint32_t fixture, oldSystem; PPCFunc *oldNoOp, *oldApplicationStep;
        ~Restore() {
            PPC_LOOKUP_FUNC(base, noOpAddress) = oldNoOp;
            PPC_LOOKUP_FUNC(base, applicationStepAddress) = oldApplicationStep;
            memory->write32(0x82A690F8, oldSystem);
            resetDeveloperTools();
            memory->release(fixture);
        }
    } restore{base, fixture, oldSystem, oldNoOp, oldApplicationStep};
    std::memset(base + fixture, 0, 0x8000);
    application = fixture; manager = fixture + 0x2000;
    const auto managerTable = fixture + 0x2100, systemObject = fixture + 0x2300,
               systemTable = fixture + 0x2400, unrelatedTable = fixture + 0x2600;
    memory->write32(application, 0x82051EA0);
    memory->write32(application + 4912, manager);
    memory->write32(manager, managerTable);
    memory->write32(managerTable + 84, noOpAddress);
    memory->write32(systemObject, systemTable);
    memory->write32(systemTable + 36, noOpAddress);
    memory->write32(0x82A690F8, systemObject);
    PPC_LOOKUP_FUNC(base, noOpAddress) = originalStep;
    PPC_LOOKUP_FUNC(base, applicationStepAddress) = originalStep;
    // The console service exists, while app+60 remains uninitialized. The
    // original general update exits before platform/heap work, but the outer
    // Mod update still executes both manager+84 and application+172 callbacks.
    // Only those virtual callbacks are isolated from unstarted game services;
    // the actual 820C5D30 frame and its original 820F68D8 entry guard execute.
    check(canLoadDeveloperMission(base, 0), "fixture console is not ready without a world client");
    check(memory->read32(0x82051EA0 + 144) == 0x820C5D30 &&
          memory->read32(0x82055458 + 144) == 0x820C5D30 &&
          memory->read32(0x82051EA0 + 172) == applicationStepAddress,
          "developer update wrapper differs from the shipped Mod update tables");
    auto input = ctx;
    input.r3.u64 = application;
    resetDeveloperTools();
    check(requestDeveloperMission("NY1_Tunnel:ny1+layer1"), "update fixture mission request failed");
    requireQueued = true; originalCalls = 0;
    auto expected = input;
    __imp__sub_820C5D30(expected, base);
    check(originalCalls == 2 && developerSnapshot().status == "Mission load queued.",
          "baseline original update consumed a native developer command");
    const auto queued = developerSnapshot().revision;
    auto actual = input;
    originalCalls = 0;
    sub_820C5D30(actual, base);
    check(originalCalls == 2 && std::memcmp(&actual, &expected, sizeof(actual)) == 0,
          "native developer update changed the original returned PPC context");
    check(!developerSnapshot().canLoadMission && !developerSnapshot().hasActivePlayer &&
          developerSnapshot().revision > queued &&
          developerSnapshot().status.starts_with("Mission loading is unavailable."),
          "uninitialized frontend consumed a mission without rejecting readiness");

    // A table with identical virtual entries is insufficient: only the two
    // authored Mod application tables are accepted by the native boundary.
    std::memcpy(base + unrelatedTable, base + 0x82051EA0, 256);
    memory->write32(application, unrelatedTable);
    resetDeveloperTools();
    check(requestDeveloperMission("NY1_Tunnel:ny1+layer1"), "guard fixture mission request failed");
    const auto beforeGuard = developerSnapshot();
    originalCalls = 0; actual = input;
    sub_820C5D30(actual, base);
    check(originalCalls == 2 && developerSnapshot().revision == beforeGuard.revision &&
          developerSnapshot().status == "Mission load queued.",
          "unrelated application table reached the native developer boundary");
    // The original preload callback sets its byte before deferred loading.
    memory->write32(application, 0x82055458);
    auto flagCall = input;
    sub_82102D38(flagCall, base);
    check(PPC_LOAD_U8(application + 4908) == 1, "original preload callback did not set its application flag");
    originalCalls = 0; actual = input;
    sub_820C5D30(actual, base);
    check(originalCalls == 2 && !developerSnapshot().canLoadMission &&
          developerSnapshot().status.starts_with("Mission loading is unavailable."),
          "retail alternate application table or pending preload bypassed readiness");
    // Base applications use the general update directly. Mod calls above
    // also traverse that wrapper, and their queued-command checks prove it
    // does not process before the complete outer update has returned.
    memory->write32(application, 0x82060D88);
    check(memory->read32(0x82060D88 + 144) == 0x820F68D8,
          "base application update differs from the shipped table");
    resetDeveloperTools();
    check(requestDeveloperMission("NY1_Tunnel:ny1+layer1"), "base update fixture mission request failed");
    expected = input;
    __imp__sub_820F68D8(expected, base);
    check(developerSnapshot().status == "Mission load queued.", "original base update consumed a native request");
    actual = input;
    sub_820F68D8(actual, base);
    check(std::memcmp(&actual, &expected, sizeof(actual)) == 0 &&
          developerSnapshot().status.starts_with("Mission loading is unavailable.") &&
          !developerSnapshot().canLoadMission,
          "base application wrapper lost original context or bypassed frontend readiness");
    requireQueued = false;
    puts("Developer update: complete original Mod/base boundaries, returned contexts, retail application tables and no-client frontend guards passed.");
}
