#include "app/windows/display_settings.h"
#include "runtime/native/fov_settings.h"
#include "runtime/native/draw_distance.h"
#include "runtime/native/display_mode.h"
#include "app/windows/display_options.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static bool approximatelyEqual(double a, double b) { return std::abs(a - b) < 0.00002; }
static std::string fileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot read isolated settings fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main() {
    struct RestoreNativeVideoMode {
        NativeVideoMode previous = nativeVideoMode();
        ~RestoreNativeVideoMode() { (void)setNativeVideoMode(previous.width, previous.height); }
    };
    [[maybe_unused]] RestoreNativeVideoMode restoreNativeVideoMode;
    const auto path = std::filesystem::temp_directory_path() /
        (L"DarkRecomp-display-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    struct Cleanup { std::filesystem::path path; bool owned = false; ~Cleanup() { if (owned) { std::error_code ec; std::filesystem::remove(path, ec); } } } cleanup{path};
    try {
        check(!std::filesystem::exists(path), "test path already exists");
        cleanup.owned = true;
        check(fieldOfViewSetting() == 0 && loadFieldOfView(path) == 0, "Original default");
        check(loadGraphicsSettings(path) == GraphicsSettings{}, "graphics defaults on missing file");
        check(!GraphicsSettings{}.reduceLightFlicker, "light flicker reduction must be opt-in");
        for (bool enabled : {true, false}) {
            auto selected = GraphicsSettings{};
            selected.reduceLightFlicker = enabled;
            selected.aspectMode = DisplayAspectMode::Stretch4x3;
            selected.subtitleScalePercent = 200;
            check(setGraphicsSettings(selected) && graphicsSettings() == selected &&
                  saveDisplaySettings(path, 0, selected) && loadGraphicsSettings(path) == selected,
                  "light flicker setting changed adjacent packed preferences or did not persist");
        }
        for (const auto bad : {L"-1", L"2", L"true", L"1.0", L"42949672960", static_cast<const wchar_t*>(nullptr)}) {
            WritePrivateProfileStringW(L"Display", L"ReduceLightFlicker", bad, path.c_str());
            auto expected = GraphicsSettings{};
            expected.aspectMode = DisplayAspectMode::Stretch4x3;
            expected.subtitleScalePercent = 200;
            check(loadGraphicsSettings(path) == expected,
                  "invalid or missing light flicker setting did not recover independently to Off");
        }
        check(GraphicsSettings{}.aspectMode == DisplayAspectMode::Auto,
              "new installs must preserve automatic display aspect");
        // Unsupported saved heights recover independently. Higher resolutions
        // use native GPU scaling within the original guest allocation limits.
        check(saveDisplaySettings(path, 114, {120, 720, true, true, false}), "seed recovery preferences");
        for (const auto height : {L"2161", L"4096", L"16384"}) {
            WritePrivateProfileStringW(L"Display", L"RenderHeight", height, path.c_str());
            check(loadGraphicsSettings(path) == GraphicsSettings{120,720,true,true,false} && loadFieldOfView(path) == 114,
                  "unsafe saved resolution did not recover independently");
        }
        for (const NativeDisplaySize expected : {NativeDisplaySize{1720,720}, {2576,1080}, {3440,1440}}) {
            const auto render = renderSizeForDisplay({3440,1440}, expected.height);
            check(render.width == expected.width && render.height == expected.height,
                  "supported native resolution did not preserve the aligned guest viewport");
        }
        // The physical ultrawide display hits the host width cap at 2160p.
        // The former even-only mode was 1364x570, while the original camera
        // produced 1364x568 and every startup video draw failed the native gate.
        const auto cappedUltrawide = renderSizeForDisplay({3440,1440}, 2160);
        const auto cappedScale = nativeResolutionScale(cappedUltrawide);
        check(cappedUltrawide.width == 4092 && cappedUltrawide.height == 1704 && cappedScale == 3 &&
              cappedUltrawide.width / cappedScale == 1364 && cappedUltrawide.height / cappedScale == 568,
              "capped ultrawide mode differs from the original fullscreen camera viewport");
        const auto native4k = renderSizeForDisplay({3840,2160}, 2160);
        check(native4k.width == 3840 && native4k.height == 2160, "4K resolution was silently downscaled");
        const auto superwide = renderSizeForDisplay({5120,1440}, 2160);
        check(superwide.width == 4096 && superwide.height == 1152, "32:9 width limit changed aspect");
        for (const NativeDisplaySize output : {NativeDisplaySize{1920,1080}, {3440,1440}, {3840,2160}, {5120,1440}}) {
            for (uint32_t height = 180; height <= 2160; ++height) {
                const auto render = renderSizeForDisplay(output, height);
                const auto scale = nativeResolutionScale(render);
                check(scale >= 1 && scale <= 3 && render.width % (4 * scale) == 0 && render.height % (4 * scale) == 0,
                      "native scaling did not retain exact integer edges and four-pixel guest viewports");
                check(render.width / scale <= 2560 && render.height / scale <= 720,
                      "native scaling exceeded the guest allocation limits");
            }
        }
        for (const NativeDisplaySize output : {NativeDisplaySize{1920,1080}, {3440,1440}, {3840,2160}, {5120,1440}}) {
            for (uint32_t height = 180; height <= 2160; ++height) {
                const auto render = renderSizeForDisplay(output, height, DisplayAspectMode::Stretch4x3);
                const auto scale = nativeResolutionScale(render);
                check(render.width * 3 == render.height * 4,
                      "4:3 stretch render dimensions lost their exact aspect");
                check(scale >= 1 && scale <= 3 && render.width % (4 * scale) == 0 &&
                      render.height % (4 * scale) == 0 && render.width / scale <= 2560 &&
                      render.height / scale <= 720,
                      "4:3 stretch dimensions violate guest viewport alignment or allocation limits");
            }
        }
        const auto untouchedVideoMode = nativeVideoMode();
        check(!setNativeVideoMode(240, 180) &&
              nativeVideoMode().width == untouchedVideoMode.width &&
              nativeVideoMode().height == untouchedVideoMode.height,
              "native video-mode gate accepted or published a too-narrow guest mode");
        for (const uint32_t requestedHeight : {180u, 239u}) {
            const auto render = renderSizeForDisplay({1920,1080}, requestedHeight,
                                                     DisplayAspectMode::Stretch4x3);
            const auto scale = nativeResolutionScale(render);
            const uint32_t guestWidth = render.width / scale;
            const uint32_t guestHeight = render.height / scale;
            check(guestWidth == 320 && guestHeight == 240,
                  "sub-240p 4:3 request did not select the smallest supported guest mode");
            check(setNativeVideoMode(guestWidth, guestHeight),
                  "4:3 helper returned a mode rejected by the native video-mode gate");
            const auto applied = nativeVideoMode();
            check(applied.width == guestWidth && applied.height == guestHeight,
                  "accepted low-resolution 4:3 mode was not published exactly");
        }
        for (const auto [height, width] : {std::pair{360u,480u}, {480u,640u}, {720u,960u},
                                           {1080u,1440u}, {1440u,1920u}, {2160u,2880u}}) {
            const auto render = renderSizeForDisplay({3440,1440}, height, DisplayAspectMode::Stretch4x3);
            check(render.width == width && render.height == height,
                  "standard 4:3 resolution changed unexpectedly");
        }
        const auto rounded4x3 = renderSizeForDisplay({1920,1080}, 1475, DisplayAspectMode::Stretch4x3);
        check(rounded4x3.width == 1920 && rounded4x3.height == 1440,
              "4:3 sizing did not choose the largest aligned mode below a scale boundary");
        for (const auto height : {1080u,1440u,2160u}) {
            auto high = GraphicsSettings{}; high.renderHeight = height;
            check(saveDisplaySettings(path,114,high) && loadGraphicsSettings(path)==high,
                  "high internal resolution did not persist");
        }
        const GraphicsSettings custom{137, 719, true, false, false, true, AntialiasingMode::FXAA};
        check(saveDisplaySettings(path, 93.125f, custom), "save custom graphics values");
        check(loadGraphicsSettings(path) == custom && loadFieldOfView(path) == 93.125f,
              "complete graphics round trip including bloom, motion blur and antialiasing");
        for (const auto mode : {DisplayAspectMode::Auto, DisplayAspectMode::Stretch4x3}) {
            auto selected = custom; selected.aspectMode = mode;
            check(setGraphicsSettings(selected) && graphicsSettings() == selected &&
                  saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected,
                  "display aspect did not round-trip through runtime packing and settings persistence");
            wchar_t savedMode[16]{};
            GetPrivateProfileStringW(L"Display", L"AspectMode", L"missing", savedMode, 16, path.c_str());
            check(std::wstring_view(savedMode) == std::to_wstring(unsigned(mode)),
                  "display aspect mode did not save its stable numeric value");
        }
        auto autoAspect = custom; autoAspect.aspectMode = DisplayAspectMode::Auto;
        check(WritePrivateProfileStringW(L"Display", L"AspectMode", nullptr, path.c_str()) &&
              loadGraphicsSettings(path) == autoAspect,
              "legacy settings must default aspect to automatic");
        for (const auto bad : {L"-1", L"2", L"Stretch4x3", L"42949672960"}) {
            check(WritePrivateProfileStringW(L"Display", L"AspectMode", bad, path.c_str()),
                  "seed invalid aspect setting");
            check(loadGraphicsSettings(path) == autoAspect,
                  "invalid aspect setting did not recover independently to Auto");
        }
        check(saveDisplaySettings(path, 93.125f, custom) && setGraphicsSettings(custom),
              "restore settings after aspect tests");
        auto badAspect = custom; badAspect.aspectMode = DisplayAspectMode(2);
        check(!validGraphicsSettings(badAspect) && !setGraphicsSettings(badAspect) && graphicsSettings() == custom &&
              !saveDisplaySettings(path, 90, badAspect) && loadGraphicsSettings(path) == custom,
              "invalid aspect mode partially applied settings");
        check(GraphicsSettings{}.antialiasing == AntialiasingMode::Off, "antialiasing must default to Off");
        for (const auto mode : {AntialiasingMode::Off, AntialiasingMode::FXAA, AntialiasingMode::SMAA,
                               AntialiasingMode::MSAA2x, AntialiasingMode::MSAA4x, AntialiasingMode::MSAA8x}) {
            auto selected = custom; selected.antialiasing = mode;
            check(saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected &&
                  loadFieldOfView(path) == 93.125f, "antialiasing mode did not persist independently");
            wchar_t savedMode[16]{};
            GetPrivateProfileStringW(L"Display", L"Antialiasing", L"missing", savedMode, 16, path.c_str());
            check(std::wstring_view(savedMode) == std::to_wstring(unsigned(mode)),
                  "antialiasing did not save the stable numeric mode");
        }
        auto aaOff = custom; aaOff.antialiasing = AntialiasingMode::Off;
        check(saveDisplaySettings(path, 93.125f, aaOff) && loadGraphicsSettings(path) == aaOff,
              "antialiasing Off did not persist independently");
        check(WritePrivateProfileStringW(L"Display", L"Antialiasing", L"1", path.c_str()) &&
              loadGraphicsSettings(path) == custom, "legacy antialiasing 1 must retain FXAA");
        check(WritePrivateProfileStringW(L"Display", L"Antialiasing", L"0", path.c_str()) &&
              loadGraphicsSettings(path) == aaOff, "legacy antialiasing 0 must retain Off");
        check(WritePrivateProfileStringW(L"Display", L"Antialiasing", nullptr, path.c_str()), "remove antialiasing for legacy settings");
        check(loadGraphicsSettings(path) == aaOff, "legacy settings must default antialiasing to Off");
        for (const auto bad : {L"-1", L"6", L"255", L"FXAA", L"1.0", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"Antialiasing", bad, path.c_str());
            check(loadGraphicsSettings(path) == aaOff, "invalid antialiasing did not fall back independently");
        }
        check(saveDisplaySettings(path, 93.125f, custom), "restore FXAA after invalid values");
        check(setGraphicsSettings(custom), "select valid antialiasing before invalid-mode checks");
        for (unsigned mode : {6u, 7u, 255u}) {
            auto invalid = custom; invalid.antialiasing = AntialiasingMode(mode);
            check(!validGraphicsSettings(invalid) && !setGraphicsSettings(invalid) && graphicsSettings() == custom &&
                  !saveDisplaySettings(path, 90, invalid) && loadGraphicsSettings(path) == custom &&
                  loadFieldOfView(path) == 93.125f, "invalid antialiasing mode partially applied settings");
        }
        check(GraphicsSettings{}.anisotropyLevels == 16, "new settings must default to 16x anisotropic filtering");
        for (unsigned levels : {1u, 2u, 4u, 8u, 16u}) {
            auto selected = custom; selected.anisotropyLevels = levels;
            check(setGraphicsSettings(selected) && graphicsSettings() == selected,
                  "anisotropic filtering packing changed another preference");
            check(saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected,
                  "anisotropic filtering selection did not persist");
        }
        check(WritePrivateProfileStringW(L"Display", L"AnisotropyLevels", nullptr, path.c_str()),
              "remove anisotropy for legacy settings");
        check(loadGraphicsSettings(path) == custom, "legacy settings must default anisotropic filtering to 16x");
        for (const auto bad : {L"0", L"3", L"15", L"17", L"-1", L"16x", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"AnisotropyLevels", bad, path.c_str());
            check(loadGraphicsSettings(path) == custom, "invalid anisotropic filtering changed another preference");
        }
        check(saveDisplaySettings(path, 93.125f, custom), "restore anisotropic filtering");
        for (unsigned levels : {0u, 3u, 15u, 17u, 32u}) {
            auto invalid = custom; invalid.anisotropyLevels = levels;
            check(!setGraphicsSettings(invalid) && graphicsSettings() == custom &&
                  !saveDisplaySettings(path, 90, invalid) && loadGraphicsSettings(path) == custom,
                  "invalid anisotropic filtering partially applied settings");
        }
        check(GraphicsSettings{}.subtitleScalePercent == 100, "subtitles must default to the original size");
        for (unsigned scale : {50u, 73u, 90u, 100u, 110u, 137u, 150u, 200u}) {
            auto selected = custom; selected.subtitleScalePercent = scale;
            check(setGraphicsSettings(selected) && graphicsSettings() == selected,
                  "subtitle scale packing changed another preference");
            check(saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected &&
                  loadFieldOfView(path) == 93.125f, "subtitle scale did not persist independently");
            wchar_t savedScale[16]{};
            GetPrivateProfileStringW(L"Display", L"SubtitleScalePercent", L"missing", savedScale, 16, path.c_str());
            check(std::wstring_view(savedScale) == std::to_wstring(scale), "subtitle scale did not save its percentage");
        }
        check(WritePrivateProfileStringW(L"Display", L"SubtitleScalePercent", nullptr, path.c_str()),
              "remove subtitle scale for legacy settings");
        check(loadGraphicsSettings(path) == custom, "legacy settings must preserve original subtitle size");
        for (const auto bad : {L"0", L"49", L"201", L"-1", L"150%", L"1.50", L"42949672960"}) {
            WritePrivateProfileStringW(L"Display", L"SubtitleScalePercent", bad, path.c_str());
            check(loadGraphicsSettings(path) == custom, "invalid subtitle scale changed another preference");
        }
        check(saveDisplaySettings(path, 93.125f, custom) && setGraphicsSettings(custom), "restore original subtitle size");
        for (unsigned scale : {0u, 49u, 201u, 256u, 0xffffffffu}) {
            auto invalid = custom; invalid.subtitleScalePercent = scale;
            check(!validGraphicsSettings(invalid) && !setGraphicsSettings(invalid) && graphicsSettings() == custom &&
                  !saveDisplaySettings(path, 90, invalid) && loadGraphicsSettings(path) == custom &&
                  loadFieldOfView(path) == 93.125f, "invalid subtitle scale partially applied settings");
        }
        // The retired native power curve has no mapping to the original
        // profile's gamma calibration. Loading ignores its key without
        // rewriting the file; unrelated saves preserve the inert value.
        wchar_t legacyGamma[32]{};
        GetPrivateProfileStringW(L"Display", L"GammaPercent", L"missing", legacyGamma, 32, path.c_str());
        check(std::wstring_view(legacyGamma) == L"missing", "fresh settings created a retired gamma key");
        for (unsigned gamma = 50; gamma <= 150; ++gamma) {
            const auto text = std::to_wstring(gamma);
            check(WritePrivateProfileStringW(L"Display", L"GammaPercent", text.c_str(), path.c_str()), "seed legacy gamma");
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
            const auto beforeLoad = fileBytes(path);
            check(loadGraphicsSettings(path) == custom && loadFieldOfView(path) == 93.125f,
                  "retired gamma changed graphics settings or FOV");
            check(fileBytes(path) == beforeLoad, "loading retired gamma rewrote the settings file");
            check(saveDisplaySettings(path, 93.125f, custom), "save settings with inert legacy gamma");
            GetPrivateProfileStringW(L"Display", L"GammaPercent", L"missing", legacyGamma, 32, path.c_str());
            check(std::wstring_view(legacyGamma) == text, "saving settings replaced the inert legacy gamma value");
        }
        for (const auto bad : {L"49", L"151", L"-1", L"nan", L"1.00", L"42949672960"}) {
            check(WritePrivateProfileStringW(L"Display", L"GammaPercent", bad, path.c_str()), "seed malformed retired gamma");
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
            const auto beforeLoad = fileBytes(path);
            check(loadGraphicsSettings(path) == custom && fileBytes(path) == beforeLoad,
                  "malformed retired gamma changed settings or rewrote the file");
        }
        for (unsigned brightness : {50u, 95u, 100u, 125u, 200u}) {
            auto selected = custom; selected.brightnessPercent = brightness;
            check(saveDisplaySettings(path, 93.125f, selected) && loadGraphicsSettings(path) == selected,
                  "brightness did not persist independently of other settings");
        }
        const auto legacyBrightness = custom;
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
        for (const auto language : {GameLanguage::System, GameLanguage::English, GameLanguage::German,
                                   GameLanguage::French, GameLanguage::Spanish, GameLanguage::Italian}) {
            check(saveDisplaySettings(path, 100.125f, custom, language) && loadGameLanguage(path) == language &&
                  loadGraphicsSettings(path) == custom && loadFieldOfView(path) == 100.125f,
                  "language and display selection did not persist together");
        }
        check(saveDisplaySettings(path, 100.125f, custom) && loadGameLanguage(path) == GameLanguage::Italian,
              "display-only save replaced the stored language");
        check(!saveDisplaySettings(path, 90, aaOff, GameLanguage(2)) &&
              loadGameLanguage(path) == GameLanguage::Italian && loadGraphicsSettings(path) == custom &&
              loadFieldOfView(path) == 100.125f, "invalid language partially saved the selection");
        // A failed final replacement must preserve both sections, even after
        // the staged copy has successfully received all new preferences.
        {
            const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(locked != INVALID_HANDLE_VALUE, "cannot lock isolated settings fixture");
            struct Unlock { HANDLE handle; ~Unlock() { CloseHandle(handle); } } unlock{locked};
            check(!saveDisplaySettings(path, 90, aaOff, GameLanguage::German),
                  "locked destination unexpectedly accepted a settings replacement");
        }
        check(loadGameLanguage(path) == GameLanguage::Italian && loadGraphicsSettings(path) == custom &&
              loadFieldOfView(path) == 100.125f, "failed settings replacement partially changed preferences");
        GetPrivateProfileStringW(L"Other", L"Preserve", L"", unrelated, 8, path.c_str());
        check(std::wstring_view(unrelated) == L"yes", "language save replaced an unrelated section");
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
        // Draw distance: Near when nothing is saved, persisted beside the other
        // display settings and preserved when those are saved again.
        check(kDefaultDrawDistance == 1500 && loadDrawDistance(path) == kDefaultDrawDistance &&
              drawDistanceSetting() == 0, "draw distance defaults to Near; the runtime starts at Original until set");
        check(saveDrawDistance(path, 1500) && loadDrawDistance(path) == 1500, "draw distance round trip");
        check(saveDisplaySettings(path, 0, GraphicsSettings{}) && loadDrawDistance(path) == 1500,
              "saving display settings preserves draw distance");
        check(!saveDrawDistance(path, kMaximumDrawDistance + 1) && loadDrawDistance(path) == 1500,
              "out-of-range draw distance is rejected");
        for (const auto text : {L"-5", L"1e3", L"65536", L"abc"}) {
            WritePrivateProfileStringW(L"Display", L"DrawDistance", text, path.c_str());
            check(loadDrawDistance(path) == kDefaultDrawDistance, "invalid saved draw distance falls back to the default");
        }
        check(saveDrawDistance(path, 0) && loadDrawDistance(path) == 0, "Original saves as zero");
        check(setDrawDistanceSetting(2000) && drawDistanceSetting() == 2000 &&
              !setDrawDistanceSetting(kMaximumDrawDistance + 1) && drawDistanceSetting() == 2000,
              "draw distance setting validates");
        setDrawDistanceSetting(0);
        {
            // Unlimited generic/object ranges become the configured distance.
            uint32_t flags = 0x40000000u; float generic = 0, object = 0;
            applyDrawDistance(flags, generic, object, 1000);
            check(flags == (0x40000000u | kClientRangeGeneric | kClientRangeObject | kClientRangeAny) &&
                  generic == 1000000.f && object == 1000000.f, "draw distance enables generic and object ranges");
            // A level's shorter range stays; a longer one is shortened.
            flags = kClientRangeGeneric | kClientRangeObject | kClientRangeAny;
            generic = 500.f * 500.f; object = 4000.f * 4000.f;
            applyDrawDistance(flags, generic, object, 1000);
            check(generic == 250000.f && object == 1000000.f, "draw distance keeps shorter level ranges");
            // Original leaves the context untouched, including character bits.
            flags = 0x08; generic = 7; object = 9;
            applyDrawDistance(flags, generic, object, 0);
            check(flags == 0x08 && generic == 7 && object == 9, "Original draw distance changes nothing");
        }
        std::puts("Display settings: persistence, validation, bloom, motion blur, FOV and draw distance passed; no game or renderer used.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
