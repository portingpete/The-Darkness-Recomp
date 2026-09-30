#pragma once
#include "developer_tools.h"

namespace DarkRecomp::Native {
// Empty for unknown IDs. Only authored retail destinations can form commands.
std::string developerMissionCommand(std::string_view id, bool initializeSession = false);
bool canLoadDeveloperMission(uint8_t* base, uint32_t client);
bool loadDeveloperMission(PPCContext& ctx, uint8_t* base, uint32_t client,
                          std::string_view id, std::string& status);
}
