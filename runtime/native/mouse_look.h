#pragma once
#include "input.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace DarkRecomp::Native {
struct MouseLookPacket { int16_t pitch = 0, yaw = 0; };
struct MouseLookPackets {
    // Over 23,000 degrees in one batch. This bound rejects corrupt/device-error
    // counts without allocating or filling the guest queue indefinitely.
    std::array<MouseLookPacket, 128> packets{};
    unsigned count = 0;
    bool rejected = false;
};

// Original relative-look commands store 1/65536 turn per signed short.
// Retain sub-unit fractions across batches instead of creating a dead zone.
class MouseLookQuantizer {
public:
    void reset() { pitch_ = yaw_ = 0; }
    MouseLookPackets convert(const MouseLookDelta& delta, bool invertY) {
        MouseLookPackets result;
        constexpr double unitsPerCount = 8.0;
        const double yaw = double(delta.x) * delta.sensitivity * unitsPerCount + yaw_;
        // Positive guest pitch looks up; Win32 positive relative Y moves down.
        const double pitch = double(delta.y) * delta.sensitivity * unitsPerCount * (invertY ? 1 : -1) + pitch_;
        constexpr double bound = 32767.0 * result.packets.size();
        if (!std::isfinite(yaw) || !std::isfinite(pitch) || std::abs(yaw) > bound || std::abs(pitch) > bound) {
            reset(); result.rejected = true; return result;
        }
        int64_t y = int64_t(yaw), p = int64_t(pitch);
        yaw_ = yaw - double(y); pitch_ = pitch - double(p);
        result.count = unsigned((std::max(std::abs(y), std::abs(p)) + 32766) / 32767);
        for (unsigned i = 0; i < result.count; ++i) {
            const int64_t remaining = result.count - i;
            const int64_t stepY = y / remaining, stepP = p / remaining;
            result.packets[i] = {int16_t(stepP), int16_t(stepY)};
            y -= stepY; p -= stepP;
        }
        return result;
    }
private:
    double pitch_ = 0, yaw_ = 0;
};
}
