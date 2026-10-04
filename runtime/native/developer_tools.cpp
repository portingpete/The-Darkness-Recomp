#include "developer_tools.h"
#include "developer_missions.h"
#include "developer_player.h"
#include "developer_invincibility.h"
#include "developer_darkness.h"
#include "developer_player_lookup.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace DarkRecomp::Native {
namespace {
std::mutex stateMutex;
DeveloperSnapshot state;
std::optional<std::string> pendingMission;
bool playerToolsEnabled = false;
bool pendingDarknessUnlock = false;
bool pendingDarknessLevel = false;
uint64_t requestRevision = 0;

void requested(std::string_view status) {
    state.status = status;
    ++state.revision;
    ++requestRevision;
}

void publish(uint64_t request, bool canLoad, bool hasPlayer, std::string_view status, bool playerApplied = false) {
    std::lock_guard lock(stateMutex);
    const bool availabilityChanged = state.canLoadMission != canLoad || state.hasActivePlayer != hasPlayer;
    state.canLoadMission = canLoad;
    state.hasActivePlayer = hasPlayer;
    if (playerApplied && status.empty() &&
        (state.status == "Player speed queued." || state.status == "Invincibility queued." ||
         state.status == "Noclip queued."))
        status = "Player settings applied.";
    const bool statusChanged = requestRevision == request && !status.empty() && state.status != status;
    if (statusChanged) state.status = status;
    if (availabilityChanged || statusChanged) ++state.revision;
}
}

DeveloperSnapshot developerSnapshot() {
    std::lock_guard lock(stateMutex);
    return state;
}

bool requestDeveloperMission(std::string_view id) {
    const auto missions = developerMissions();
    if (std::none_of(missions.begin(), missions.end(), [id](const auto& mission) { return mission.id == id; })) return false;
    std::lock_guard lock(stateMutex);
    // One pending transition. Replacing it avoids loading a succession of
    // worlds if the user changes their selection before the next engine tick.
    pendingMission = id;
    requested("Mission load queued.");
    return true;
}

bool requestDeveloperSpeed(float multiplier) {
    if (!std::isfinite(multiplier) || multiplier < 0.25f || multiplier > 4.0f) return false;
    std::lock_guard lock(stateMutex);
    state.playerSpeed = multiplier;
    playerToolsEnabled = true;
    requested("Player speed queued.");
    return true;
}

void requestDeveloperInvincibility(bool enabled) {
    std::lock_guard lock(stateMutex);
    state.invincible = enabled;
    playerToolsEnabled = true;
    requested("Invincibility queued.");
}

void requestDeveloperNoclip(bool enabled) {
    std::lock_guard lock(stateMutex);
    state.noclip = enabled;
    playerToolsEnabled = true;
    requested("Noclip queued.");
}

void requestDeveloperUnlockDarkness() {
    std::lock_guard lock(stateMutex);
    pendingDarknessUnlock = true;
    requested("Darkness ability unlock queued.");
}

void requestDeveloperMaxDarkness() {
    std::lock_guard lock(stateMutex);
    pendingDarknessLevel = true;
    requested("Maximum Darkness level queued.");
}

void resetDeveloperTools() {
    std::lock_guard lock(stateMutex);
    state = {};
    pendingMission.reset();
    playerToolsEnabled = false;
    pendingDarknessUnlock = pendingDarknessLevel = false;
    ++requestRevision;
    resetDeveloperPlayer();
}

bool processDeveloperTools(PPCContext& ctx, uint8_t* base, uint32_t client, bool missionReady) {
    std::optional<std::string> mission;
    float speed;
    bool invincible, noclip, enabled, unlockDarkness, maxDarkness;
    uint64_t request;
    {
        std::lock_guard lock(stateMutex);
        mission = std::move(pendingMission);
        pendingMission.reset();
        speed = state.playerSpeed;
        invincible = state.invincible;
        noclip = state.noclip;
        enabled = playerToolsEnabled;
        unlockDarkness = std::exchange(pendingDarknessUnlock, false);
        maxDarkness = std::exchange(pendingDarknessLevel, false);
        request = requestRevision;
    }
    const bool canLoad = missionReady && canLoadDeveloperMission(base, client);
    // Original console calls get an independent register context and the same
    // floating-point mode as an ordinary guest function invocation.
    auto call = ctx;
    const unsigned savedMath = _mm_getcsr();
    struct RestoreMath {
        unsigned saved;
        ~RestoreMath() { _mm_setcsr(saved); }
    } restoreMath{savedMath};
    _mm_setcsr(savedMath & ~(_MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK));
    if (mission) {
        updateDeveloperPlayer(call, base, 0, 1, false, false);
        std::string status;
        const bool loaded = canLoad && loadDeveloperMission(call, base, client, *mission, status);
        if (!canLoad) status = "Mission loading is unavailable. Try again when the main menu or game is ready.";
        publish(request, loaded ? false : canLoad, false, status);
        // Even a failed engine command may have changed transition state.
        return canLoad;
    }
    const auto player = updateDeveloperPlayer(call, base, client, speed, invincible, enabled, noclip);
    if (unlockDarkness || maxDarkness) {
        const auto handles = resolveDeveloperPlayer(call, base, client);
        const auto darkness = applyDeveloperDarkness(call, base, handles, unlockDarkness, maxDarkness);
        publish(request, canLoad, player.hasActivePlayer, darkness.status);
    } else publish(request, canLoad, player.hasActivePlayer, player.status, player.applied);
    return false;
}
}
