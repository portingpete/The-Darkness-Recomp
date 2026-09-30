#include "language_settings.h"
#include <windows.h>
#include <atomic>
#include <cstdio>

namespace DarkRecomp::Native {
namespace {
std::atomic<GameLanguage> selected{GameLanguage::System};
bool equals(std::wstring_view text, std::wstring_view expected) noexcept {
    if (text.size() != expected.size()) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i] >= L'A' && text[i] <= L'Z' ? text[i] + (L'a' - L'A') : text[i];
        if (c != expected[i]) return false;
    }
    return true;
}
}
bool validGameLanguage(GameLanguage language) noexcept {
    switch (language) {
    case GameLanguage::System: case GameLanguage::English: case GameLanguage::German:
    case GameLanguage::French: case GameLanguage::Spanish: case GameLanguage::Italian: return true;
    default: return false;
    }
}
bool parseGameLanguage(std::wstring_view text, GameLanguage& language) noexcept {
    const struct { GameLanguage language; std::wstring_view code, name; } choices[]{
        {GameLanguage::System, L"auto", L"system"},
        {GameLanguage::English, L"en", L"english"},
        {GameLanguage::German, L"de", L"german"},
        {GameLanguage::French, L"fr", L"french"},
        {GameLanguage::Spanish, L"es", L"spanish"},
        {GameLanguage::Italian, L"it", L"italian"}
    };
    for (const auto& choice : choices) {
        if (equals(text, choice.code) || equals(text, choice.name)) {
            language = choice.language;
            return true;
        }
    }
    return false;
}
std::wstring_view gameLanguageName(GameLanguage language) noexcept {
    switch (language) {
    case GameLanguage::System: return L"auto";
    case GameLanguage::English: return L"en";
    case GameLanguage::German: return L"de";
    case GameLanguage::French: return L"fr";
    case GameLanguage::Spanish: return L"es";
    case GameLanguage::Italian: return L"it";
    default: return {};
    }
}
GameLanguage gameLanguageSetting() noexcept { return selected.load(std::memory_order_relaxed); }
bool setGameLanguageSetting(GameLanguage language) noexcept {
    if (!validGameLanguage(language)) return false;
    selected.store(language, std::memory_order_relaxed);
    return true;
}
uint32_t consoleLanguageFor(GameLanguage language, uint16_t windowsUiLanguage) noexcept {
    if (validGameLanguage(language) && language != GameLanguage::System) return uint32_t(language);
    switch (PRIMARYLANGID(windowsUiLanguage)) {
    case LANG_GERMAN: return uint32_t(GameLanguage::German);
    case LANG_FRENCH: return uint32_t(GameLanguage::French);
    case LANG_SPANISH: return uint32_t(GameLanguage::Spanish);
    case LANG_ITALIAN: return uint32_t(GameLanguage::Italian);
    default: return uint32_t(GameLanguage::English);
    }
}
uint32_t configuredConsoleLanguage() noexcept {
    return consoleLanguageFor(gameLanguageSetting(), GetUserDefaultUILanguage());
}
GameLanguage loadGameLanguage(const std::filesystem::path& path) noexcept {
    wchar_t text[32]{};
    const DWORD length = GetPrivateProfileStringW(L"Game", L"Language", L"auto", text, 32, path.c_str());
    GameLanguage language = GameLanguage::System;
    if (length >= 31 || !parseGameLanguage(text, language))
        std::fputs("[Language] Invalid saved language; using Windows UI language (English fallback).\n", stderr);
    return language;
}
bool saveGameLanguage(const std::filesystem::path& path, GameLanguage language) noexcept {
    if (!validGameLanguage(language)) return false;
    return WritePrivateProfileStringW(L"Game", L"Language", gameLanguageName(language).data(), path.c_str()) != FALSE;
}
}
