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
        check(saveGameLanguage(path, GameLanguage::English), "restore English language selection");
        for (const auto bad : {L"", L"1", L"2", L"en-US", L"english!", L" en", L"en ", L"japanese"}) {
            GameLanguage parsed = GameLanguage::French;
            check(!parseGameLanguage(bad, parsed) && parsed == GameLanguage::French,
                  "invalid language text changed the caller's selection");
        }
        const auto invalid = static_cast<GameLanguage>(2);
        check(!setGameLanguageSetting(invalid) && gameLanguageSetting() == GameLanguage::Italian,
              "unsupported retail language ID changed runtime selection");
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
        setGameLanguageSetting(GameLanguage::System);
        std::puts("Language settings: explicit host-independent IDs, automatic regional fallback, strict parsing and persistence passed.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
