#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace DarkRecomp::Native {
// Explicit reference retains the original-menu wrappers in the static library.
void initializeVideoSettingsMenu() noexcept;
bool changeVideoSetting(std::string_view action, int direction) noexcept;
std::string videoSettingLabel(std::string_view action);
bool isNativeMenuSlider(std::string_view action) noexcept;
float nativeMenuSliderPosition(std::string_view action) noexcept;
bool setNativeMenuSliderPosition(std::string_view action, float position) noexcept;
std::string nativeMenuSliderCaption(std::string_view action);
bool setNativeMenuSliderPointer(uint8_t* base, uint32_t button, int32_t absoluteX);
bool isNativeMenuSliderButton(uint8_t* base, uint32_t button);
// Guest callbacks queue host UI work; Windows owns the display/window thread.
bool activateNativeMenuAction(std::string_view action) noexcept;
bool takeExitGameRequest() noexcept;
// Mouse preferences are saved independently of graphics and keyboard bindings.
bool takeMouseSensitivitySaveRequest() noexcept;
void reportMouseSensitivitySave(bool saved) noexcept;
}
