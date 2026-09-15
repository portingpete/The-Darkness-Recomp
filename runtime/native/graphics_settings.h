#pragma once
#include <cstdint>
#include <string_view>

namespace DarkRecomp::Native {
constexpr unsigned kMaximumRenderHeight = 2160;
struct GraphicsSettings {
    unsigned frameRateLimit = 60; // Zero is uncapped.
    unsigned renderHeight = 720; // Applied at startup, with the display aspect.
    bool verticalSync = false;
    bool fullscreen = true;
    bool bloom = true;
    bool motionBlur = false;
    bool antialiasing = false; // FXAA at presentation; applies without a restart.
    unsigned gammaPercent = 100; // 50..150; 100 is neutral, lower is darker.
    unsigned brightnessPercent = 100; // 50..200; final image intensity, 100 is neutral.
    bool operator==(const GraphicsSettings&) const = default;
};
bool validGraphicsSettings(const GraphicsSettings&) noexcept;
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
