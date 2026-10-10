#include "app/windows/mouse_settings.h"
#include "runtime/native/mouse_sensitivity.h"
#include <windows.h>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot read isolated mouse settings fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::wstring savedSensitivity(const std::filesystem::path& path) {
    wchar_t text[32]{};
    GetPrivateProfileStringW(L"Mouse", L"Sensitivity", L"missing", text, 32, path.c_str());
    return text;
}
void saveFixture(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(output.good(), "cannot create isolated mouse settings fixture");
    output.write(text.data(), std::streamsize(text.size()));
    check(output.good(), "cannot seed isolated mouse settings fixture");
}
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"DarkRecomp-mouse-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    struct Cleanup {
        std::filesystem::path path; bool owned = false;
        ~Cleanup() { if (owned) { std::error_code error; std::filesystem::remove(path, error); } }
    } cleanup{path};
    try {
        check(!std::filesystem::exists(path), "isolated mouse fixture already exists");
        cleanup.owned = true;
        check(loadMouseSensitivity(path) == kDefaultMouseSensitivity && !std::filesystem::exists(path),
              "missing mouse setting did not default to 1 without creating a file");
        float parsed = 7.25f;
        for (const auto valid : {L"0.1", L".1", L"1", L"5", L"10", L"001.250"})
            check(parseMouseSensitivity(valid, parsed), "valid decimal sensitivity rejected");
        check(parsed == 1.25f, "fractional decimal sensitivity changed value");
        for (const auto bad : {L"", L".", L"0", L"0.09999999999999", L"10.000000000001",
                               L"20", L"100", L"101", L"nan", L"NaN", L"inf", L"Infinity",
                               L"-1", L"+1", L"1e1", L"1,25", L"1.", L"1..25", L"1oops",
                               L" 1", L"1 ", L"1\t", L"99999999999999999"})
            check(!parseMouseSensitivity(bad, parsed) && parsed == 1.25f,
                  "invalid decimal sensitivity was accepted or changed output");

        const std::string preserved =
            "; Retain comments and unrelated preferences.\r\n"
            "[Display]\r\nFieldOfView=93.125\r\nRenderHeight=720\r\n\r\n"
            "[Keyboard]\r\nBindingsVersion=2\r\nJumpPrimary=69\r\nJumpSecondary=32\r\n\r\n"
            "[Other]\r\nPreserve=alpha beta\r\nOpaque=any ; text=unchanged\r\n\r\n";
        saveFixture(path, preserved);
        check(loadMouseSensitivity(path) == kDefaultMouseSensitivity && bytes(path) == preserved,
              "missing Mouse section changed the file or default");
        check(WritePrivateProfileStringW(L"Mouse", L"OtherOption", L"keep this value", path.c_str()),
              "cannot seed unrelated mouse preference");
        for (const float selected : {kMinimumMouseSensitivity, 1.f, 5.f, kMaximumMouseSensitivity,
                                     1.23456789f, 0.100123457f, std::nextafter(kMaximumMouseSensitivity, 0.f),
                                     std::nextafter(kMinimumMouseSensitivity, 1.f)}) {
            check(saveMouseSensitivity(path, selected) && loadMouseSensitivity(path) == selected,
                  "mouse sensitivity did not round-trip the complete float value");
            const auto saved = savedSensitivity(path);
            check(saved.find(L',') == std::wstring::npos && saved.find_first_of(L"eE") == std::wstring::npos &&
                  parseMouseSensitivity(saved, parsed) && parsed == selected,
                  "saved sensitivity is not a locale-independent decimal");
            check(bytes(path).find(preserved) == 0,
                  "mouse save changed unrelated sections, comments or arbitrary INI values");
            wchar_t otherOption[32]{};
            GetPrivateProfileStringW(L"Mouse", L"OtherOption", L"", otherOption, 32, path.c_str());
            check(std::wstring_view(otherOption) == L"keep this value", "mouse save replaced another mouse key");
        }

        for (const auto legacy : {L"10.000000000001", L"20", L"20.5", L"100", L"100.000"}) {
            check(WritePrivateProfileStringW(L"Mouse", L"Sensitivity", legacy, path.c_str()),
                  "cannot seed legacy sensitivity");
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
            const auto beforeLoad = bytes(path);
            check(loadMouseSensitivity(path) == 10.f && bytes(path) == beforeLoad,
                  "legacy sensitivity was not capped at 10 without rewriting the INI");
            const float beforeParse = parsed;
            check(!parseMouseSensitivity(legacy, parsed) && parsed == beforeParse,
                  "strict parsing accepted a value above the current cap");
        }

        // Exercise decimal serialization under an installed comma locale when
        // available; the decimal parser must remain strict in any locale.
        {
            const char* currentLocale = std::setlocale(LC_NUMERIC, nullptr);
            const std::string originalLocale = currentLocale ? currentLocale : "C";
            struct RestoreLocale {
                const std::string& original;
                ~RestoreLocale() { std::setlocale(LC_NUMERIC, original.c_str()); }
            } restore{originalLocale};
            for (const auto locale : {"French_France.1252", "fr_FR.UTF-8", "German_Germany.1252"})
                if (std::setlocale(LC_NUMERIC, locale)) break;
            check(parseMouseSensitivity(L"5.5", parsed) && parsed == 5.5f &&
                  !parseMouseSensitivity(L"5,5", parsed), "process locale affected decimal parsing");
            check(saveMouseSensitivity(path, 1.23456789f) && loadMouseSensitivity(path) == 1.23456789f &&
                  savedSensitivity(path) == L"1.23456788", "locale or formatting lost fractional precision");
        }
        const auto saved = bytes(path);
        for (const float bad : {-1.f, 0.f, 20.f, 100.f, std::nextafter(kMinimumMouseSensitivity, 0.f),
                               std::nextafter(kMaximumMouseSensitivity, std::numeric_limits<float>::infinity()),
                               std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()})
            check(!saveMouseSensitivity(path, bad) && bytes(path) == saved,
                  "invalid sensitivity save changed the file");
        {
            const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(locked != INVALID_HANDLE_VALUE, "cannot lock isolated mouse fixture");
            struct Unlock { HANDLE handle; ~Unlock() { CloseHandle(handle); } } unlock{locked};
            check(!saveMouseSensitivity(path, 5.f) && bytes(path) == saved &&
                  loadMouseSensitivity(path) == 1.23456789f,
                  "failed final replacement changed existing mouse or unrelated settings");
        }
        for (const auto malformed : {L"", L"0", L"0.09999999999999", L"100.000000000001", L"nan",
                                     L"inf", L"1e1", L"5,5", L"1.", L"not-a-number",
                                     L"1234567890123456789012345678901234567890"}) {
            check(WritePrivateProfileStringW(L"Mouse", L"Sensitivity", malformed, path.c_str()),
                  "cannot seed corrupt sensitivity");
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
            const auto corrupt = bytes(path);
            check(loadMouseSensitivity(path) == kDefaultMouseSensitivity && bytes(path) == corrupt,
                  "corrupt sensitivity did not default to 1 without rewriting the INI");
        }
        const auto corrupt = bytes(path);
        {
            const auto originalAttributes = GetFileAttributesW(path.c_str());
            check(originalAttributes != INVALID_FILE_ATTRIBUTES &&
                  SetFileAttributesW(path.c_str(), originalAttributes | FILE_ATTRIBUTE_READONLY),
                  "cannot make isolated mouse fixture read-only");
            struct RestoreAttributes {
                const std::filesystem::path& path; DWORD attributes;
                ~RestoreAttributes() { SetFileAttributesW(path.c_str(), attributes); }
            } restore{path, originalAttributes};
            check(loadMouseSensitivity(path) == kDefaultMouseSensitivity &&
                  !saveMouseSensitivity(path, 5.f) && bytes(path) == corrupt,
                  "read-only corrupt sensitivity was rewritten by load or failed save");
        }
        check(saveMouseSensitivity(path, kDefaultMouseSensitivity) &&
              loadMouseSensitivity(path) == kDefaultMouseSensitivity && bytes(path).find(preserved) == 0,
              "mouse settings could not recover after failed writes");
        std::puts("Mouse settings: strict decimal validation, legacy migration, complete float persistence and atomic INI preservation passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mouse settings: %s\n", error.what());
        return 1;
    }
}
