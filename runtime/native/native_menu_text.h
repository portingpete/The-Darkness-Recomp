#pragma once
#include <cstdint>
#include <string_view>

namespace DarkRecomp::Native {
// Russian dumps supply CP1251 fonts in the retail English content slot.
// Latch the verified revision and effective language before opening menus.
void initializeNativeMenuText(bool russianRevision, uint32_t consoleLanguage) noexcept;
bool russianNativeMenus() noexcept;
std::string_view nativeMenuText(std::string_view english) noexcept;
}
