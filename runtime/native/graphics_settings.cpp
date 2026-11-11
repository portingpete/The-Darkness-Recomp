#include "graphics_settings.h"
#include <atomic>

namespace DarkRecomp::Native {
namespace {
constexpr uint64_t pack(GraphicsSettings s) {
    // Bits 27..29 hold AA; brightness, filtering, subtitle scale and aspect follow.
    return s.frameRateLimit | (s.renderHeight << 10) |
           (uint32_t(s.verticalSync) << 23) | (uint32_t(s.fullscreen) << 24) |
           (uint32_t(s.bloom) << 25) | (uint32_t(s.motionBlur) << 26) |
           (uint64_t(s.antialiasing) << 27) | (uint64_t(s.brightnessPercent) << 30) |
           (uint64_t(s.anisotropyLevels) << 38) | (uint64_t(s.subtitleScalePercent) << 43) |
           (uint64_t(s.aspectMode) << 51) | (uint64_t(s.reduceLightFlicker) << 52);
}
std::atomic<uint64_t> settings{pack({})};
std::atomic<bool> savePending{false}, saveFailed{false};
}
bool validGraphicsSettings(const GraphicsSettings& s) noexcept {
    return s.frameRateLimit <= 1000 && s.renderHeight >= 180 && s.renderHeight <= kMaximumRenderHeight &&
           validAntialiasingMode(s.antialiasing) && validDisplayAspectMode(s.aspectMode) &&
           s.brightnessPercent >= 50 && s.brightnessPercent <= 200 &&
           validAnisotropyLevels(s.anisotropyLevels) &&
           s.subtitleScalePercent >= kMinimumSubtitleScalePercent &&
           s.subtitleScalePercent <= kMaximumSubtitleScalePercent;
}
GraphicsSettings graphicsSettings() noexcept {
    const auto bits = settings.load(std::memory_order_relaxed);
    return {unsigned(bits & 1023), unsigned((bits >> 10) & 8191), bool(bits & (1u << 23)), bool(bits & (1u << 24)),
            bool(bits & (1u << 25)), bool(bits & (1u << 26)), AntialiasingMode((bits >> 27) & 7),
            unsigned((bits >> 30) & 255), unsigned((bits >> 38) & 31), unsigned((bits >> 43) & 255),
            DisplayAspectMode((bits >> 51) & 1), bool((bits >> 52) & 1)};
}
bool setGraphicsSettings(const GraphicsSettings& s) noexcept {
    if (!validGraphicsSettings(s)) return false;
    settings.store(pack(s), std::memory_order_relaxed);
    return true;
}
void requestDisplaySettingsSave() noexcept { savePending.store(true); }
bool takeDisplaySettingsSaveRequest() noexcept { return savePending.exchange(false); }
void reportDisplaySettingsSave(bool success) noexcept { saveFailed.store(!success); }
bool displaySettingsSaveFailed() noexcept { return saveFailed.load(); }
}
