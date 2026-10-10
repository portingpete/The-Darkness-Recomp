#pragma once
#include <algorithm>
#include <cmath>

namespace DarkRecomp::Native {
inline constexpr float kMinimumMouseSensitivity = 0.1f;
inline constexpr float kMaximumMouseSensitivity = 10.f;
inline constexpr float kDefaultMouseSensitivity = 1.f;

inline bool isValidMouseSensitivity(float value) noexcept {
    return std::isfinite(value) && value >= kMinimumMouseSensitivity && value <= kMaximumMouseSensitivity;
}

// Adjust in 0.1x steps throughout the range.
// Clamp at either end so an extra press cannot jump to the opposite extreme.
inline float stepMouseSensitivity(float value, int direction) noexcept {
    if (!isValidMouseSensitivity(value) || (direction != -1 && direction != 1)) return value;
    double next;
    if (direction > 0)
        next = (std::floor(double(value) * 10 + 0.00001) + 1) / 10;
    else
        next = (std::ceil(double(value) * 10 - 0.00001) - 1) / 10;
    return std::clamp(float(next), kMinimumMouseSensitivity, kMaximumMouseSensitivity);
}
}
