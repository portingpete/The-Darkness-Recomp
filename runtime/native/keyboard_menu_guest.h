#pragma once
#include <cstdint>
#include <string_view>

struct PPCContext;
namespace DarkRecomp::Native {
// Called only at original guest UI boundaries; disk IO stays on the host.
bool activateKeyboardMenu(PPCContext& ctx, uint8_t* base, uint32_t button,
                          std::string_view action);
void updateKeyboardMenuGuest(PPCContext& ctx, uint8_t* base, uint32_t frontend);
}
