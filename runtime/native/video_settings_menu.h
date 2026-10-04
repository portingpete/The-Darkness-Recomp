#pragma once
#include <string>
#include <string_view>

namespace DarkRecomp::Native {
// Explicit reference retains the original-menu wrappers in the static library.
void initializeVideoSettingsMenu() noexcept;
bool changeVideoSetting(std::string_view action, int direction) noexcept;
std::string videoSettingLabel(std::string_view action);
// Guest callbacks queue host UI work; Windows owns the display/window thread.
bool activateNativeMenuAction(std::string_view action) noexcept;
bool takeExitGameRequest() noexcept;
}
