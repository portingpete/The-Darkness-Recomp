#pragma once
#include <cstdint>
#include <string_view>

namespace DarkRecomp::Native {
constexpr unsigned kMaximumRenderHeight = 2160;
enum class AntialiasingMode : uint8_t {
    Off = 0, FXAA = 1, SMAA = 2, MSAA2x = 3, MSAA4x = 4, MSAA8x = 5,
};
constexpr bool validAntialiasingMode(AntialiasingMode mode) noexcept {
    return unsigned(mode) <= unsigned(AntialiasingMode::MSAA8x);
}
constexpr unsigned antialiasingSampleCount(AntialiasingMode mode) noexcept {
    switch (mode) {
    case AntialiasingMode::MSAA2x: return 2;
    case AntialiasingMode::MSAA4x: return 4;
    case AntialiasingMode::MSAA8x: return 8;
    default: return 1;
    }
}
constexpr std::string_view antialiasingModeLabel(AntialiasingMode mode) noexcept {
    switch (mode) {
    case AntialiasingMode::FXAA: return "FXAA";
    case AntialiasingMode::SMAA: return "SMAA";
    case AntialiasingMode::MSAA2x: return "MSAA 2x";
    case AntialiasingMode::MSAA4x: return "MSAA 4x";
    case AntialiasingMode::MSAA8x: return "MSAA 8x";
    default: return "Off";
    }
}
struct GraphicsSettings {
    unsigned frameRateLimit = 60; // Zero is uncapped.
    unsigned renderHeight = 720; // Applied at startup, with the display aspect.
    bool verticalSync = false;
    bool fullscreen = true;
    bool bloom = true;
    bool motionBlur = false;
    AntialiasingMode antialiasing = AntialiasingMode::Off; // Applies without a restart.
    unsigned brightnessPercent = 100; // 50..200; final image intensity, 100 is neutral.
    unsigned anisotropyLevels = 16; // 1 preserves original filtering; otherwise 2, 4, 8 or 16.
    bool operator==(const GraphicsSettings&) const = default;
};
bool validGraphicsSettings(const GraphicsSettings&) noexcept;
constexpr bool validAnisotropyLevels(unsigned levels) noexcept {
    return levels == 1 || levels == 2 || levels == 4 || levels == 8 || levels == 16;
}
GraphicsSettings graphicsSettings() noexcept;
bool setGraphicsSettings(const GraphicsSettings&) noexcept;

// Select existing final-composite permutations, preserving exposure, color
// mapping, velocity generation and the Darkness radial effects.
constexpr uint32_t graphicsFragmentFlags(std::string_view name, uint32_t flags,
                                         bool bloom = true, bool motionBlur = false) noexcept {
    return name == "XREngine_Final5" || name == "XREngine_Final4"
        ? flags & ~((motionBlur ? 0u : 1u) | (bloom ? 0u : 8u)) : flags;
}

// Guest menu changes are persisted by the host window thread.
void requestDisplaySettingsSave() noexcept;
bool takeDisplaySettingsSaveRequest() noexcept;
void reportDisplaySettingsSave(bool success) noexcept;
bool displaySettingsSaveFailed() noexcept;
}
