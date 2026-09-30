#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

struct PPCContext;

namespace DarkRecomp::Native {
struct DeveloperMission {
    std::string_view id;
    std::string_view title;
};
std::span<const DeveloperMission> developerMissions() noexcept;
void initializeDeveloperTools() noexcept;

// Host UI exchanges values only. Guest objects are accessed on the engine
// thread after the original application update has completed.
struct DeveloperSnapshot {
    bool canLoadMission = false;
    bool hasActivePlayer = false;
    float playerSpeed = 1;
    bool invincible = false;
    std::string status = "Waiting for the game.";
    uint64_t revision = 0;
};
DeveloperSnapshot developerSnapshot();
bool requestDeveloperMission(std::string_view id);
bool requestDeveloperSpeed(float multiplier);
void requestDeveloperInvincibility(bool enabled);
void resetDeveloperTools();

// Returns true after a mission command: the caller must not reuse its old
// client/player pointers during that update.
bool processDeveloperTools(PPCContext& ctx, uint8_t* base, uint32_t client, bool missionReady = true);
}
