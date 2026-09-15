#include "app/windows/display_settings.h"
#include "runtime/native/fov_settings.h"
#include "app/windows/display_options.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static bool approximatelyEqual(double a, double b) { return std::abs(a - b) < 0.00002; }
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"DarkRecomp-display-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    struct Cleanup { std::filesystem::path path; bool owned = false; ~Cleanup() { if (owned) { std::error_code ec; std::filesystem::remove(path, ec); } } } cleanup{path};
    try {
        check(!std::filesystem::exists(path), "test path already exists");
        cleanup.owned = true;
        check(fieldOfViewSetting() == 0 && loadFieldOfView(path) == 0, "Original default");
        check(loadGraphicsSettings(path) == GraphicsSettings{}, "graphics defaults on missing file");
        // Unsupported saved heights recover independently. Higher resolutions
        // use native GPU scaling within the original guest allocation limits.
        check(saveDisplaySettings(path, 114, {120, 720, true, true, false}), "seed recovery preferences");
        for (const auto height : {L"2161", L"4096", L"16384"}) {
            WritePrivateProfileStringW(L"Display", L"RenderHeight", height, path.c_str());
            check(loadGraphicsSettings(path) == GraphicsSettings{120,720,true,true,false} && loadFieldOfView(path) == 114,
                  "unsafe saved resolution did not recover independently");
        }
        for (const auto requested : {720u, 1080u, 1440u}) {
            const auto render = renderSizeForDisplay({3440,1440}, requested);
            check(render.width == requested * 3440 / 1440 && render.height == requested,
                  "supported native resolution was silently downscaled");
        }
        const auto native4k = renderSizeForDisplay({3840,2160}, 2160);
        check(native4k.width == 3840 && native4k.height == 2160, "4K resolution was silently downscaled");
        const auto superwide = renderSizeForDisplay({5120,1440}, 2160);
        check(superwide.width == 4096 && superwide.height == 1152, "32:9 width limit changed aspect");
        for (const NativeDisplaySize output : {NativeDisplaySize{1920,1080}, {3440,1440}, {3840,2160}, {5120,1440}}) {
            for (uint32_t height = 180; height <= 2160; ++height) {
                const auto render = renderSizeForDisplay(output, height);
                const auto scale = nativeResolutionScale(render);
                check(scale >= 1 && scale <= 3 && render.width % (2 * scale) == 0 && render.height % (2 * scale) == 0,
                      "native scaling did not retain exact integer edges and even guest dimensions");
                check(render.width / scale <= 2560 && render.height / scale <= 720,
                      "native scaling exceeded the guest allocation limits");
            }
        }
        for (const auto height : {1080u,1440u,2160u}) {
            auto high = GraphicsSettings{}; high.renderHeight = height;
            check(saveDisplaySettings(path,114,high) && loadGraphicsSettings(path)==high,
                  "high internal resolution did not persist");
        }
        const GraphicsSettings custom{137, 719, true, false, false, true, true};
        check(saveDisplaySettings(path, 93.125f, custom), "save custom graphics values");
        check(loadGraphicsSettings(path) == custom && loadFieldOfView(path) == 93.125f,
              "complete graphics round trip including bloom, motion blur and antialiasing");
        auto aaOff = custom; aaOff.antialiasing = false;
        check(saveDisplaySettings(path, 93.125f, aaOff) && loadGraphicsSettings(path) == aaOff,
              "antialiasing Off did not persist independently");
        check(saveDisplaySettings(path, 93.125f, custom), "restore FXAA");
        check(WritePrivateProfileStringW(L"Display", L"Antialiasing", nullptr, path.c_str()), "remove antialiasing for legacy settings");
        check(loadGraphicsSettings(path) == aaOff, "legacy settings must default antialiasing to Off");
        for (const auto bad : {L"-1", L"2", L"FXAA", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"Antialiasing", bad, path.c_str());
            check(loadGraphicsSettings(path) == aaOff, "invalid antialiasing did not fall back independently");
        }
        check(saveDisplaySettings(path, 93.125f, custom), "restore FXAA after invalid values");
        for (unsigned gamma : {50u, 85u, 100u, 150u}) {
            auto selected = custom; selected.gammaPercent = gamma;
            check(saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected,
                  "gamma did not persist independently");
        }
        WritePrivateProfileStringW(L"Display", L"GammaPercent", nullptr, path.c_str());
        check(loadGraphicsSettings(path) == custom, "legacy settings must default gamma to neutral");
        for (const auto bad : {L"49", L"151", L"-1", L"nan", L"1.00", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"GammaPercent", bad, path.c_str());
            check(loadGraphicsSettings(path) == custom, "invalid gamma did not fall back independently");
        }
        check(saveDisplaySettings(path, 93.125f, custom), "restore gamma after invalid values");
        auto badGamma = custom; badGamma.gammaPercent = 0;
        check(!saveDisplaySettings(path, 90, badGamma) && loadGraphicsSettings(path) == custom && loadFieldOfView(path)==93.125f,
              "invalid gamma save changed existing preferences");
        for (unsigned brightness : {50u, 95u, 100u, 125u, 200u}) {
            auto selected = custom; selected.gammaPercent = 85; selected.brightnessPercent = brightness;
            check(saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected,
                  "brightness did not persist independently of gamma and other settings");
        }
        auto legacyBrightness = custom; legacyBrightness.gammaPercent = 85;
        WritePrivateProfileStringW(L"Display", L"BrightnessPercent", nullptr, path.c_str());
        check(loadGraphicsSettings(path) == legacyBrightness, "legacy settings must default brightness to neutral");
        for (const auto bad : {L"49", L"201", L"-1", L"nan", L"100%", L"1.00", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"BrightnessPercent", bad, path.c_str());
            check(loadGraphicsSettings(path) == legacyBrightness, "invalid brightness changed another preference");
        }
        check(saveDisplaySettings(path, 93.125f, custom), "restore brightness after invalid values");
        auto badBrightness = custom; badBrightness.brightnessPercent = 201;
        check(!saveDisplaySettings(path, 90, badBrightness) && loadGraphicsSettings(path) == custom && loadFieldOfView(path)==93.125f,
              "invalid brightness save changed existing preferences");
        auto blurOff = custom; blurOff.motionBlur = false;
        check(saveDisplaySettings(path, 93.125f, blurOff) && loadGraphicsSettings(path) == blurOff,
              "motion blur Off did not persist independently of bloom");
        check(saveDisplaySettings(path, 93.125f, custom), "restore motion blur On");
        check(WritePrivateProfileStringW(L"Display", L"MotionBlur", nullptr, path.c_str()), "remove new key for legacy settings");
        check(loadGraphicsSettings(path) == blurOff, "legacy settings must default motion blur to Off");
        check(saveDisplaySettings(path, 93.125f, custom), "restore complete settings");
        for (const auto bad : {L"-1", L"1001", L"60fps", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"FrameRateLimit", bad, path.c_str());
            auto expected = custom; expected.frameRateLimit = 60;
            check(loadGraphicsSettings(path) == expected, "invalid frame cap falls back independently");
        }
        check(saveDisplaySettings(path, 93.125f, custom), "restore custom settings");
        for (const auto key : {L"RenderHeight", L"VerticalSync", L"Fullscreen", L"Bloom", L"MotionBlur", L"Antialiasing"})
            WritePrivateProfileStringW(L"Display", key, L"invalid", path.c_str());
        auto fallback = GraphicsSettings{}; fallback.frameRateLimit = 137;
        check(loadGraphicsSettings(path) == fallback, "invalid graphics values use safe defaults");
        check(saveDisplaySettings(path, 93.125f, custom), "restore settings after corruption test");
        auto invalid = custom; invalid.renderHeight = 2161;
        check(!saveDisplaySettings(path, 90, invalid) && loadGraphicsSettings(path) == custom &&
              loadFieldOfView(path) == 93.125f, "invalid save leaves complete selection intact");
        float value = 77;
        check(parseFieldOfView(L"90.5", value) && value == 90.5f, "fractional FOV parsing");
        for (const auto bad : {L"", L"nan", L"inf", L"59.9", L"120.1", L"90oops", L"90,5", L"-1", L"90.", L"120.00000000000001"})
            check(!parseFieldOfView(bad, value) && value == 90.5f, "invalid FOV must not mutate output");
        check(setFieldOfViewSetting(90.5f), "set FOV");
        for (const float bad : {-1.f, 59.9f, 120.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
            check(!setFieldOfViewSetting(bad) && fieldOfViewSetting() == 90.5f, "invalid FOV must preserve setting");
        check(WritePrivateProfileStringW(L"Other", L"Preserve", L"yes", path.c_str()), "seed unrelated setting");
        check(saveDisplaySettings(path, 100.125f, custom) && loadFieldOfView(path) == 100.125f, "persist display settings");
        wchar_t unrelated[8]{};
        GetPrivateProfileStringW(L"Other", L"Preserve", L"", unrelated, 8, path.c_str());
        check(std::wstring_view(unrelated) == L"yes", "saving display setting preserves other sections");
        check(!saveFieldOfView(path, 121) && loadFieldOfView(path) == 100.125f, "invalid write preserves file");
        check(WritePrivateProfileStringW(L"Display", L"FieldOfView", L"not-a-number", path.c_str()), "seed corrupt config");
        check(loadFieldOfView(path) == 0, "corrupt config uses Original");
        check(saveFieldOfView(path, 0) && loadFieldOfView(path) == 0, "persist Original");
        const double original = guestFovToHorizontal16By9(90, 4.0 / 3.0);
        check(approximatelyEqual(original, 106.2602047083), "original projection convention");
        check(approximatelyEqual(horizontal16By9ToGuestFov(original, 4.0 / 3.0), 90), "projection conversion round trip");
        constexpr double radians = 3.14159265358979323846 / 360;
        for (float configured : {60.f, 90.f, 120.f}) {
            const float unzoomed = relativeGuestFovDegrees(90, float(original), configured);
            const float zoomed = relativeGuestFovDegrees(35, float(original), configured);
            check(approximatelyEqual(std::tan(zoomed * radians) / std::tan(unzoomed * radians),
                       std::tan(35 * radians) / std::tan(90 * radians)), "FOV setting preserves zoom tangent ratio");
            for (const double aspect : {4.0 / 3.0, 16.0 / 9.0, 43.0 / 18.0, 32.0 / 9.0}) {
                const double wide = horizontalFovAtAspect(configured, 16.0 / 9.0, aspect);
                check(approximatelyEqual(std::tan(wide * radians) / aspect,
                           std::tan(configured * radians) / (16.0 / 9.0)), "ultrawide preserves vertical FOV");
            }
        }
        check(relativeGuestFovDegrees(35, float(original), 0) == 35, "Original retains zoom");
        check(relativeGuestFovDegrees(35, 0, 90) == 35, "unproven baseline cannot alter camera");
        setFieldOfViewSetting(0);
        setGraphicsSettings({});
        std::puts("Display settings: persistence, validation, bloom, motion blur and FOV passed; no game or renderer used.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
