#include "graphics_settings.h"
#include <atomic>

namespace DarkRecomp::Native {
namespace {
constexpr uint64_t pack(GraphicsSettings s) {
    return s.frameRateLimit | (s.renderHeight << 10) |
           (uint32_t(s.verticalSync) << 23) | (uint32_t(s.fullscreen) << 24) |
           (uint32_t(s.bloom) << 25) | (uint32_t(s.motionBlur) << 26) |
           (uint32_t(s.antialiasing) << 27) | (uint64_t(s.gammaPercent) << 28) |
           (uint64_t(s.brightnessPercent) << 36);
}
std::atomic<uint64_t> settings{pack({})};
std::atomic<bool> savePending{false}, saveFailed{false};
}
bool validGraphicsSettings(const GraphicsSettings& s) noexcept {
    return s.frameRateLimit <= 1000 && s.renderHeight >= 180 && s.renderHeight <= kMaximumRenderHeight &&
           s.gammaPercent >= 50 && s.gammaPercent <= 150 &&
           s.brightnessPercent >= 50 && s.brightnessPercent <= 200;
}
GraphicsSettings graphicsSettings() noexcept {
    const auto bits = settings.load(std::memory_order_relaxed);
    return {unsigned(bits & 1023), unsigned((bits >> 10) & 8191), bool(bits & (1u << 23)), bool(bits & (1u << 24)),
            bool(bits & (1u << 25)), bool(bits & (1u << 26)), bool(bits & (1u << 27)), unsigned((bits >> 28) & 255),
            unsigned((bits >> 36) & 255)};
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
