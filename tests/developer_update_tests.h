#pragma once
#include "runtime/native/developer_missions.h"
#include "runtime/native/menu_pointer.h"
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_820C5D30);
extern "C" PPC_FUNC(__imp__sub_820F68D8);
extern "C" PPC_FUNC(__imp__sub_82102D38);

namespace DeveloperUpdateFixture {
constexpr uint32_t noOpAddress = 0x827AAB08;
constexpr uint32_t applicationStepAddress = 0x820FF0B0;
inline uint32_t application, manager;
inline unsigned originalCalls;
inline bool requireQueued;
inline unsigned lookupRequests;

static PPC_FUNC(requestDefaultsDuringLookup) {
    ++lookupRequests;
    check(requestDeveloperSpeed(1), "Concurrent cleanup request failed");
    // The service lookup leaves its already-zero reference unchanged.
}

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
    // Original820F4460 installs CubeFrontEnd at application+3672;
    // original8237CAC8 stores the application back-reference at FrontEnd+24.
    // Keep a different CWorldData reference at +60 to catch using that field.
    const auto frontend = fixture + 0x3000, worldData = fixture + 0x5000;
    memory->write32(application + 60, worldData);
    memory->write32(application + 3672, frontend);
    memory->write32(frontend, 0x82071A10);
    memory->write32(frontend + 24, application);
    check(guestGameplayLoadFrontend(base, application) == frontend,
          "Accepted load confused CWorldData with its owning CubeFrontEnd");
    memory->write32(frontend + 24, manager);
    check(guestGameplayLoadFrontend(base, application) == 0,
          "Foreign frontend accepted the application's load request");
    memory->write32(frontend + 24, application);
    memory->write32(frontend, unrelatedTable);
    check(guestGameplayLoadFrontend(base, application) == 0,
          "Unrelated frontend type accepted a gameplay load");
    memory->write32(frontend, 0x82071A10);
    memory->write32(application + 3672, 0xfffffffcu);
    check(guestGameplayLoadFrontend(base, application) == 0,
          "Malformed frontend address reached a load ownership read");
    memory->write32(application + 60, 0);
    memory->write32(application + 3672, 0);
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
    setDeveloperToolsVisible(false);
    resetDeveloperTools();
    requireQueued = false;
    // The original exits its general update before touching this client
    // array. The idle native tail must also leave the guarded page alone.
    const auto guarded = fixture + 0x7000;
    memory->write32(application + 3668, guarded);
    DWORD guardedProtection;
    check(VirtualProtect(base + guarded, 4096, PAGE_NOACCESS, &guardedProtection),
          "Idle developer fixture guard setup failed");
    struct RestoreGuard {
        uint8_t* page; DWORD protection;
        ~RestoreGuard() { DWORD ignored; VirtualProtect(page, 4096, protection, &ignored); }
    } restoreGuard{base + guarded, guardedProtection};
    auto idleExpected = input;
    originalCalls = 0;
    __imp__sub_820C5D30(idleExpected, base);
    const auto idleRevision = developerSnapshot().revision;
    auto idleActual = input;
    originalCalls = 0;
    sub_820C5D30(idleActual, base);
    check(originalCalls == 2 && std::memcmp(&idleActual, &idleExpected, sizeof(idleActual)) == 0 &&
          developerSnapshot().revision == idleRevision && !developerToolsNeedsUpdate(),
          "Closed idle developer wrapper inspected guarded clients or changed the original update");
    memory->write32(application + 3668, 0xfffffffc);
    originalCalls = 0; idleActual = input;
    sub_820C5D30(idleActual, base);
    check(originalCalls == 2 && developerSnapshot().revision == idleRevision,
          "Closed idle developer wrapper inspected malformed client memory");
    memory->write32(application + 3668, 0);
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
    auto expectedFlagCall = input;
    __imp__sub_82102D38(expectedFlagCall, base);
    sub_82102D38(flagCall, base);
    check(PPC_LOAD_U8(application + 4908) == 1 &&
          std::memcmp(&flagCall, &expectedFlagCall, sizeof(flagCall)) == 0,
          "Preload capture hook changed the original flag or returned PPC context");
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
    // Returning to defaults needs one cleanup update even after closing the
    // panel. Requests made while processing must survive that completion.
    const auto client = fixture + 0x3000;
    memory->write32(client, 0x820807E0);
    memory->write32(client + 536, 1);
    const auto oldToken = memory->read32(0x82A40308);
    struct RestoreToken {
        uint32_t value;
        ~RestoreToken() { memory->write32(0x82A40308, value); }
    } restoreToken{oldToken};
    memory->write32(0x82A40308, 1);
    PPC_LOOKUP_FUNC(base, noOpAddress) = requestDefaultsDuringLookup;
    resetDeveloperTools();
    check(requestDeveloperSpeed(2), "Active override gate fixture request failed");
    lookupRequests = 0;
    processDeveloperTools(input, base, client, false);
    check(lookupRequests == 1 && developerSnapshot().playerSpeed == 1 && developerToolsNeedsUpdate(),
          "A default-settings request arriving during processing lost its cleanup update");
    processDeveloperTools(input, base, 0, false);
    check(!developerToolsNeedsUpdate(), "Completed default-settings cleanup did not return closed tools to idle");
    check(requestDeveloperSpeed(2), "Persistent override gate fixture request failed");
    processDeveloperTools(input, base, 0, false);
    setDeveloperToolsVisible(false);
    check(developerToolsNeedsUpdate(), "Applied player override stopped updating when the panel closed");
    requestDeveloperSpeed(1);
    requestDeveloperInvincibility(false);
    requestDeveloperNoclip(false);
    check(developerToolsNeedsUpdate(), "Restoring defaults skipped the final player-hook cleanup update");
    processDeveloperTools(input, base, 0, false);
    check(!developerToolsNeedsUpdate(), "Restored default player settings kept closed developer tools active");
    resetDeveloperTools();
    puts("Developer update: complete original Mod/base boundaries, returned contexts, retail application tables and no-client frontend guards passed.");
}
