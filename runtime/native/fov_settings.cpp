#include "fov_settings.h"

#include <atomic>
#include <cmath>
#include <limits>

namespace DarkRecomp::Native {
namespace {
std::atomic<float> configuredFov{kOriginalFov};
constexpr double kDegreesToHalfRadians = 3.14159265358979323846 / 360.0;
constexpr float kMinimumGuestFov = 0.1f;
constexpr float kMaximumGuestFov = 179.0f;

bool validAngle(double degrees) noexcept {
    return std::isfinite(degrees) && degrees > 0.0 && degrees < 180.0;
}
} // namespace

float fieldOfViewSetting() noexcept {
    return configuredFov.load(std::memory_order_relaxed);
}

bool isValidConfiguredHorizontalFovDegrees(float degrees) noexcept {
    return std::isfinite(degrees) &&
           (degrees == kOriginalFov ||
            (degrees >= kMinimumConfiguredFov && degrees <= kMaximumConfiguredFov));
}

bool setFieldOfViewSetting(float degrees) noexcept {
    if (!isValidConfiguredHorizontalFovDegrees(degrees)) return false;
    configuredFov.store(degrees == 0.0f ? kOriginalFov : degrees,
                        std::memory_order_relaxed);
    return true;
}

double horizontalFovAtAspect(double degrees, double fromAspect,
                             double toAspect) noexcept {
    if (!validAngle(degrees) || !std::isfinite(fromAspect) || fromAspect <= 0.0 ||
        !std::isfinite(toAspect) || toAspect <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    if (fromAspect == toAspect) return degrees;
    const double tangent = std::tan(degrees * kDegreesToHalfRadians) *
                           (toAspect / fromAspect);
    if (!std::isfinite(tangent) || tangent <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return std::atan(tangent) / kDegreesToHalfRadians;
}

double guestFovToHorizontal16By9(double guestDegrees,
                                double referenceAspect) noexcept {
    // 8275EF9C..8275EFB4 evaluates cos(F*pi/360); 8275EFE4..8275F054
    // reconstructs cot(F/2). 8275F070..8275F0EC supplies the reference-aspect
    // multiplier and viewport dimensions. Verified constants: 8209E0F8=pi,
    // 8209E298=1/360, 82A48E40=2. This describes the unit-scale perspective
    // path, not a projection-matrix override.
    return horizontalFovAtAspect(guestDegrees, referenceAspect, kFovReferenceAspect);
}

double horizontal16By9ToGuestFov(double horizontalDegrees,
                                double referenceAspect) noexcept {
    return horizontalFovAtAspect(horizontalDegrees, kFovReferenceAspect, referenceAspect);
}

float relativeGuestFovDegrees(float originalGuestDegrees,
                              float originalHorizontal16By9Baseline,
                              float configuredHorizontal16By9) noexcept {
    if (configuredHorizontal16By9 == kOriginalFov ||
        !isValidConfiguredHorizontalFovDegrees(configuredHorizontal16By9) ||
        !validAngle(originalHorizontal16By9Baseline) ||
        !std::isfinite(originalGuestDegrees) ||
        originalGuestDegrees < kMinimumGuestFov || originalGuestDegrees > kMaximumGuestFov)
        return originalGuestDegrees;
    if (configuredHorizontal16By9 == originalHorizontal16By9Baseline)
        return originalGuestDegrees;

    const double scale = std::tan(configuredHorizontal16By9 * kDegreesToHalfRadians) /
                         std::tan(originalHorizontal16By9Baseline * kDegreesToHalfRadians);
    const double result = std::atan(std::tan(originalGuestDegrees * kDegreesToHalfRadians) *
                                    scale) / kDegreesToHalfRadians;
    if (!std::isfinite(result) || result < kMinimumGuestFov || result > kMaximumGuestFov)
        return originalGuestDegrees;
    return static_cast<float>(result);
}

} // namespace DarkRecomp::Native
