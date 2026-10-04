#pragma once
#include <cstdint>
#include <string>

struct PPCContext;

namespace DarkRecomp::Native {
struct DeveloperPlayerHandles;

struct DeveloperDarknessResult {
    bool applied = false;
    std::string status;
};

// One-shot progression grants, on the engine thread with freshly resolved
// campaign-player handles. The original save system may retain these grants.
DeveloperDarknessResult applyDeveloperDarkness(PPCContext& ctx, uint8_t* base,
    const DeveloperPlayerHandles& player, bool unlock, bool maximum);
}
