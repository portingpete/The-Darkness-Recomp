#include "stall_profiler.h"
#include "graphics_settings.h"
#include "fov_settings.h"
#include "language_settings.h"
#include "video_settings_menu.h"
#include "keyboard_menu.h"
#include "keyboard_menu_guest.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DarkRecomp::Native {
namespace {
std::atomic<bool> exitGameRequested{false};
}
void initializeVideoSettingsMenu() noexcept {
    reportDisplaySettingsSave(true);
    endKeyboardMenu();
    exitGameRequested.store(false, std::memory_order_release);
}
bool activateNativeMenuAction(std::string_view action) noexcept {
    if (action == "darkrecomp.exit") exitGameRequested.store(true, std::memory_order_release);
    else return false;
    return true;
}
bool takeExitGameRequest() noexcept { return exitGameRequested.exchange(false, std::memory_order_acq_rel); }

namespace {
template<size_t N> unsigned step(unsigned value, int direction, const std::array<unsigned, N>& choices) {
    if (direction > 0) {
        for (auto choice : choices) if (choice > value) return choice;
        return choices.front();
    }
    for (auto it = choices.rbegin(); it != choices.rend(); ++it) if (*it < value) return *it;
    return choices.back();
}
}

bool changeVideoSetting(std::string_view action, int direction) noexcept {
    if (direction != -1 && direction != 1) return false;
    if (action == "darkrecomp.language") {
        const auto next = step(unsigned(gameLanguageSetting()), direction,
            std::array{unsigned(GameLanguage::System), unsigned(GameLanguage::English), unsigned(GameLanguage::German),
                       unsigned(GameLanguage::French), unsigned(GameLanguage::Spanish), unsigned(GameLanguage::Italian)});
        if (!setGameLanguageSetting(GameLanguage(next))) return false;
        requestDisplaySettingsSave();
        return true;
    }
    auto settings = graphicsSettings();
    if (action == "darkrecomp.bloom") settings.bloom = !settings.bloom;
    else if (action == "darkrecomp.motionblur") settings.motionBlur = !settings.motionBlur;
    else if (action == "darkrecomp.antialiasing") settings.antialiasing = !settings.antialiasing;
    else if (action == "darkrecomp.anisotropy")
        settings.anisotropyLevels = step(settings.anisotropyLevels, direction, std::array{1u, 2u, 4u, 8u, 16u});
    else if (action == "darkrecomp.brightness")
        settings.brightnessPercent = unsigned(std::clamp(int(settings.brightnessPercent) + direction * 5, 50, 200));
    else if (action == "darkrecomp.vsync") settings.verticalSync = !settings.verticalSync;
    else if (action == "darkrecomp.mode") settings.fullscreen = !settings.fullscreen;
    else if (action == "darkrecomp.resolution")
        settings.renderHeight = step(settings.renderHeight, direction, std::array{360u, 480u, 720u, 1080u, 1440u, 2160u});
    else if (action == "darkrecomp.fps")
        settings.frameRateLimit = step(settings.frameRateLimit, direction, std::array{0u, 30u, 60u, 90u, 120u, 144u, 165u, 240u, 360u});
    else if (action == "darkrecomp.fov") {
        const auto value = fieldOfViewSetting();
        const float next = direction > 0 ? (value == 0 ? 60.f : value >= 120 ? 0.f : std::floor(value) + 1.f)
                                        : (value == 0 ? 120.f : value <= 60 ? 0.f : std::ceil(value) - 1.f);
        setFieldOfViewSetting(next);
    } else return false;
    setGraphicsSettings(settings);
    requestDisplaySettingsSave();
    return true;
}

std::string videoSettingLabel(std::string_view action) {
    const auto settings = graphicsSettings();
    std::string result;
    if (action == "darkrecomp.bloom") result = settings.bloom ? "On" : "Off";
    else if (action == "darkrecomp.motionblur") result = settings.motionBlur ? "On" : "Off";
    else if (action == "darkrecomp.antialiasing") result = settings.antialiasing ? "FXAA" : "Off";
    else if (action == "darkrecomp.anisotropy")
        result = settings.anisotropyLevels == 1 ? "Original" : std::to_string(settings.anisotropyLevels) + "x";
    else if (action == "darkrecomp.brightness") result = std::to_string(settings.brightnessPercent) + "%";
    else if (action == "darkrecomp.vsync") result = settings.verticalSync ? "On" : "Off";
    else if (action == "darkrecomp.mode") result = settings.fullscreen ? "Borderless" : "Windowed";
    else if (action == "darkrecomp.resolution") result = std::to_string(settings.renderHeight) + "p";
    else if (action == "darkrecomp.fps") result = settings.frameRateLimit ? std::to_string(settings.frameRateLimit) : "Unlimited";
    else if (action == "darkrecomp.fov") {
        char fov[32]{};
        const auto value = fieldOfViewSetting();
        std::snprintf(fov, sizeof(fov), "%g", double(value));
        result = value == 0 ? "Original" : fov;
    }
    else if (action == "darkrecomp.language") {
        const auto language = gameLanguageSetting();
        // The original value column allows twelve glyphs between the arrows.
        // Use the compact System label; all explicit language names fit.
        const auto name = language == GameLanguage::System ? std::wstring_view(L"System") : gameLanguageDisplayName(language);
        result.assign(name.begin(), name.end());
    }
    if (!result.empty()) {
        // The original TEXT callback sizes the button from its first label.
        // Keep the arrows sixteen small glyphs apart for every value, so its
        // native hit rectangle remains eight cells wide without overriding it.
        if (displaySettingsSaveFailed()) result = "Save failed";
        // Match the retail menu typography for every live setting value.
        for (char& glyph : result)
            if (glyph >= 'a' && glyph <= 'z') glyph = char(glyph - 'a' + 'A');
        const size_t padding = result.size() < 12 ? 12 - result.size() : 0;
        result = "sc, < " + std::string(padding / 2, ' ') + result +
                 std::string(padding - padding / 2, ' ') + " >";
    }
    return result;
}
}

namespace {
// CStr's byte-string backing begins with a 16-bit tag. These objects are
// owned by the original UI; the wrappers inspect original window fields.
std::string_view byteString(uint8_t* base, uint32_t object) {
    const uint32_t data = PPC_LOAD_U32(object + 4);
    if (!data || (PPC_LOAD_U16(data) & 0x8000)) return {};
    const char* text = reinterpret_cast<const char*>(base + data + 2);
    const auto length = strnlen_s(text, 128);
    return length < 128 ? std::string_view(text, length) : std::string_view{};
}
std::string_view actionForButton(uint8_t* base, uint32_t button) {
    if (PPC_LOAD_U32(button) != 0x82074850) return {};
    return byteString(base, button + 344); // SCRIPT_PRESSED in CMWnd_CubeButton.
}

void replaceLabel(PPCContext& ctx, uint8_t* base, uint32_t window, const std::string& text) {
    if (text.empty() || text == byteString(base, window + 280)) return;
    // Let the original CStr implementation own/refcount the replacement.
    // Calls use a private stack frame and preserve the renderer's registers.
    auto call = ctx;
    call.r1.u64 -= 512;
    const uint32_t raw = call.r1.u32 + 80, str = call.r1.u32 + 384;
    std::memcpy(base + raw, text.c_str(), text.size() + 1);
    call.r3.u64 = str; call.r4.u64 = raw;
    sub_821F86A8(call, base);
    call.r3.u64 = window + 280; call.r4.u64 = str;
    sub_821F8BF0(call, base);
    call.r3.u64 = str;
    sub_821F8AD0(call, base);
}
void refreshLabel(PPCContext& ctx, uint8_t* base, uint32_t button) {
    const auto action = actionForButton(base, button);
    auto text = DarkRecomp::Native::keyboardMenuLabel(action);
    if (text.empty()) text = DarkRecomp::Native::videoSettingLabel(action);
    replaceLabel(ctx, base, button, text);
}
}

// Original CubeButton rendering, focus, typography and hit rectangles remain
// in charge. ALWAYSPAINT makes the engine refresh the row's current value.
extern "C" PPC_FUNC(__imp__sub_823980A8);
PPC_FUNC(sub_823980A8) {
    DarkRecomp::Native::StallProfiler::Scope stallProfile(DarkRecomp::Native::StallProfiler::Section::Guest, __func__, 0x823980A8u, uint32_t(ctx.lr));
    refreshLabel(ctx, base, ctx.r3.u32);
    __imp__sub_823980A8(ctx, base);
}

// CubeText retains its authored rectangle and stays outside the focus list.
// Its original ALWAYSPAINT path clears that whole rectangle before repainting.
extern "C" PPC_FUNC(__imp__sub_8239A4E8);
PPC_FUNC(sub_8239A4E8) {
    DarkRecomp::Native::StallProfiler::Scope stallProfile(DarkRecomp::Native::StallProfiler::Section::Guest, __func__, 0x8239A4E8u, uint32_t(ctx.lr));
    const auto window = ctx.r3.u32;
    if (PPC_LOAD_U32(window) == 0x82074100 &&
        byteString(base, window + 168) == "DARKRECOMP_KEYBOARD_STATUS") {
        replaceLabel(ctx, base, window,
            DarkRecomp::Native::keyboardMenuLabel("darkrecomp.keyboard.status"));
    }
    __imp__sub_8239A4E8(ctx, base);
}

// Original pressed callback covers both confirm and mouse activation.
extern "C" PPC_FUNC(__imp__sub_823981D8);
PPC_FUNC(sub_823981D8) {
    DarkRecomp::Native::StallProfiler::Scope stallProfile(DarkRecomp::Native::StallProfiler::Section::Guest, __func__, 0x823981D8u, uint32_t(ctx.lr));
    const auto action = actionForButton(base, ctx.r3.u32);
    if (DarkRecomp::Native::activateKeyboardMenu(ctx, base, ctx.r3.u32, action) ||
        DarkRecomp::Native::activateNativeMenuAction(action) || DarkRecomp::Native::changeVideoSetting(action, 1)) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_823981D8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_8239ECD8);
PPC_FUNC(sub_8239ECD8) {
    DarkRecomp::Native::StallProfiler::Scope stallProfile(DarkRecomp::Native::StallProfiler::Section::Guest, __func__, 0x8239ECD8u, uint32_t(ctx.lr));
    const uint32_t button = ctx.r3.u32, message = ctx.r4.u32;
    if (PPC_LOAD_U32(message) == 3 && (PPC_LOAD_U32(button + 84) & 9) == 1) {
        const auto key = PPC_LOAD_U32(message + 8);
        if (!(key & 0x8000) && ((key & 511) == 226 || (key & 511) == 227)) {
            const auto action = actionForButton(base, button);
            if (action == "darkrecomp.keybindings" || action == "darkrecomp.exit" ||
                DarkRecomp::Native::changeVideoSetting(action, (key & 511) == 226 ? -1 : 1)) {
                ctx.r3.u64 = 1;
                return;
            }
        }
    }
    __imp__sub_8239ECD8(ctx, base);
}
