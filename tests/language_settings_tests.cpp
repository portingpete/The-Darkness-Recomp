#include "runtime/native/language_settings.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>

using namespace DarkRecomp::Native;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"DarkRecomp-language-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    struct Cleanup {
        std::filesystem::path path;
        bool owned = false;
        ~Cleanup() { if (owned) { std::error_code ec; std::filesystem::remove(path, ec); } }
    } cleanup{path};
    try {
        check(!std::filesystem::exists(path), "language fixture path already exists");
        cleanup.owned = true;
        check(loadGameLanguage(path) == GameLanguage::System, "missing language config changed legacy behavior");
        check(WritePrivateProfileStringW(L"Display", L"FrameRateLimit", L"144", path.c_str()) != FALSE,
              "seed unrelated display setting");
        for (const auto language : {GameLanguage::System, GameLanguage::English, GameLanguage::German,
                                   GameLanguage::French, GameLanguage::Spanish, GameLanguage::Italian}) {
            GameLanguage parsed = GameLanguage::System;
            check(parseGameLanguage(gameLanguageName(language), parsed) && parsed == language,
                  "canonical language code failed to parse");
            check(saveGameLanguage(path, language) && loadGameLanguage(path) == language,
                  "selected language did not persist");
            check(setGameLanguageSetting(language) && gameLanguageSetting() == language,
                  "valid language selection was rejected");
            if (language != GameLanguage::System)
                check(consoleLanguageFor(language, MAKELANGID(LANG_GERMAN, SUBLANG_DEFAULT)) == uint32_t(language),
                      "explicit language still followed Windows UI language");
        }
        const struct { const wchar_t* name; GameLanguage language; } aliases[]{
            {L"SYSTEM", GameLanguage::System}, {L"English", GameLanguage::English},
            {L"GERMAN", GameLanguage::German}, {L"French", GameLanguage::French},
            {L"Spanish", GameLanguage::Spanish}, {L"ITALIAN", GameLanguage::Italian},
            {L"EN", GameLanguage::English}, {L"DE", GameLanguage::German}
        };
        for (const auto& alias : aliases) {
            GameLanguage parsed = GameLanguage::Italian;
            check(parseGameLanguage(alias.name, parsed) && parsed == alias.language,
                  "language name/code is not ASCII case insensitive");
        }
        const struct { GameLanguage language; std::wstring_view label; } labels[]{
            {GameLanguage::System, L"System default"}, {GameLanguage::English, L"English"},
            {GameLanguage::German, L"German"}, {GameLanguage::French, L"French"},
            {GameLanguage::Spanish, L"Spanish"}, {GameLanguage::Italian, L"Italian"}
        };
        for (const auto& label : labels) {
            check(gameLanguageDisplayName(label.language) == label.label, "shared language display label changed");
            for (const wchar_t c : label.label)
                check(c > 0 && c < 128, "language label contains characters outside the menu font's ASCII repertoire");
        }
        check(saveGameLanguage(path, GameLanguage::English), "restore English language selection");
        for (const auto bad : {L"", L"1", L"2", L"en-US", L"english!", L" en", L"en ", L"japanese"}) {
            GameLanguage parsed = GameLanguage::French;
            check(!parseGameLanguage(bad, parsed) && parsed == GameLanguage::French,
                  "invalid language text changed the caller's selection");
        }
        check(initializeGameLanguageSetting(GameLanguage::English) &&
              gameLanguageSetting() == GameLanguage::English && configuredConsoleLanguage() == 1,
              "startup did not latch the saved language");
        check(overrideGameLanguageForRun(GameLanguage::German) &&
              gameLanguageSetting() == GameLanguage::English && configuredConsoleLanguage() == 3,
              "command-line override changed the editable language preference");
        check(saveGameLanguage(path, gameLanguageSetting()) && loadGameLanguage(path) == GameLanguage::English,
              "saving preferences persisted the command-line language override");
        check(setGameLanguageSetting(GameLanguage::French) &&
              gameLanguageSetting() == GameLanguage::French && configuredConsoleLanguage() == 3,
              "pending menu preference changed this run's localized resources");
        check(saveGameLanguage(path, gameLanguageSetting()) && loadGameLanguage(path) == GameLanguage::French &&
              configuredConsoleLanguage() == 3,
              "pending language could not persist independently of the effective guest language");
        check(initializeGameLanguageSetting(loadGameLanguage(path)) && configuredConsoleLanguage() == 4,
              "next launch did not activate the saved language preference");
        check(initializeGameLanguageSetting(GameLanguage::System) &&
              gameLanguageSetting() == GameLanguage::System && configuredConsoleLanguage() ==
                  consoleLanguageFor(GameLanguage::System, GetUserDefaultUILanguage()),
              "automatic startup language did not resolve the Windows UI language");
        const auto systemLanguage = configuredConsoleLanguage();
        check(setGameLanguageSetting(GameLanguage::Italian) && configuredConsoleLanguage() == systemLanguage,
              "automatic effective language followed a later explicit menu preference");
        check(initializeGameLanguageSetting(gameLanguageSetting()) && configuredConsoleLanguage() == 6,
              "reinitialization did not latch the next launch's explicit language");
        check(saveGameLanguage(path, GameLanguage::English), "restore persisted English preference");
        const auto invalid = static_cast<GameLanguage>(2);
        check(!setGameLanguageSetting(invalid) && gameLanguageSetting() == GameLanguage::Italian,
              "unsupported retail language ID changed runtime selection");
        check(!initializeGameLanguageSetting(invalid) && !overrideGameLanguageForRun(invalid) &&
              gameLanguageSetting() == GameLanguage::Italian && configuredConsoleLanguage() == 6,
              "invalid startup or CLI language changed preferred/effective state");
        check(gameLanguageName(invalid).empty() && gameLanguageDisplayName(invalid).empty(),
              "unsupported language has a persistence code or UI label");
        check(!saveGameLanguage(path, invalid) && loadGameLanguage(path) == GameLanguage::English,
              "unsupported language save changed existing preferences");
        for (const auto bad : {L"japanese", L"1", L"english!", L"abcdefghijklmnopqrstuvwxyz0123456789"}) {
            check(WritePrivateProfileStringW(L"Game", L"Language", bad, path.c_str()) != FALSE,
                  "write corrupt saved language");
            check(loadGameLanguage(path) == GameLanguage::System, "invalid saved language did not fall back to automatic");
        }
        const struct { WORD host; uint32_t expected; } hosts[]{
            {MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), 1},
            {MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN_SWISS), 3},
            {MAKELANGID(LANG_FRENCH, SUBLANG_FRENCH_CANADIAN), 4},
            {MAKELANGID(LANG_SPANISH, SUBLANG_SPANISH_MEXICAN), 5},
            {MAKELANGID(LANG_ITALIAN, SUBLANG_ITALIAN_SWISS), 6},
            {MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT), 1}
        };
        for (const auto& host : hosts)
            check(consoleLanguageFor(GameLanguage::System, host.host) == host.expected,
                  "automatic language selection lost regional language mapping or English fallback");
        wchar_t other[16]{};
        GetPrivateProfileStringW(L"Display", L"FrameRateLimit", L"", other, 16, path.c_str());
        check(std::wstring_view(other) == L"144", "saving language changed display preferences");
        initializeGameLanguageSetting(GameLanguage::System);
        std::puts("Language settings: startup latch, pending preferences, run-only overrides, labels, parsing and persistence passed.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
