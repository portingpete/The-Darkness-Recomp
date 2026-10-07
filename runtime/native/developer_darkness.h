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
// campaign-player handles. Unlock includes the original Ancient gun pair and
// repairs a missing partner without adding duplicates. The save system may
// retain these grants, including a partial inventory grant if creation fails.
DeveloperDarknessResult applyDeveloperDarkness(PPCContext& ctx, uint8_t* base,
    const DeveloperPlayerHandles& player, bool unlock, bool maximum);
}
