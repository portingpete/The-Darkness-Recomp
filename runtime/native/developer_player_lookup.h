#pragma once
#include <cstdint>

struct PPCContext;
namespace DarkRecomp::Native {
struct DeveloperPlayerHandles {
    uint32_t server = 0;
    uint32_t actor = 0;
    uint32_t state = 0;
};
// Engine thread only. Uses the current client's local-player ID, never the
// camera ID; returned addresses are valid only at this update boundary.
DeveloperPlayerHandles resolveDeveloperPlayer(PPCContext& ctx, uint8_t* base, uint32_t client);
// The retail preloading flag is separate from the client's transition flags.
bool developerMissionTransitionPending(PPCContext& ctx, uint8_t* base);
}
