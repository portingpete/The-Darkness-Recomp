#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace DarkRecomp::Native {
// Retail XCONFIG language IDs for the languages supported by this title.
// System preserves the existing Windows UI language selection and fallback.
enum class GameLanguage : uint32_t {
    System = 0, English = 1, German = 3, French = 4, Spanish = 5, Italian = 6
};
bool validGameLanguage(GameLanguage language) noexcept;
bool parseGameLanguage(std::wstring_view text, GameLanguage& language) noexcept;
std::wstring_view gameLanguageName(GameLanguage language) noexcept;
std::wstring_view gameLanguageDisplayName(GameLanguage language) noexcept;
// The editable preference applies on the next launch. Guest localization
// remains on the effective language latched before game initialization.
GameLanguage gameLanguageSetting() noexcept;
bool setGameLanguageSetting(GameLanguage language) noexcept;
bool initializeGameLanguageSetting(GameLanguage language) noexcept;
// A command-line override affects only this run, not the saved preference.
bool overrideGameLanguageForRun(GameLanguage language) noexcept;
uint32_t consoleLanguageFor(GameLanguage language, uint16_t windowsUiLanguage) noexcept;
uint32_t configuredConsoleLanguage() noexcept;

// Stored beside display preferences in DarkRecomp.settings.ini as
// [Game] Language=auto|en|de|fr|es|it. CLI overrides apply only to that run.
GameLanguage loadGameLanguage(const std::filesystem::path& path) noexcept;
bool saveGameLanguage(const std::filesystem::path& path, GameLanguage language) noexcept;
}
