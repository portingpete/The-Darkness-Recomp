#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace DarkRecomp::Prompts {
// Applied after projection: xy' = scale * xy + offset * w. Only an intact
// screen rectangle may grow; callers separately verify its prompt ownership.
inline std::array<float, 4> promptQuadTransform(
        std::span<const std::array<float, 4>> clip,
        std::span<const uint16_t> indices,
        std::array<float, 4> clipBounds = {-1, -1, 1, 1}) noexcept {
    constexpr std::array<float, 4> identity{1, 0, 0, 0};
    if (clip.size() != 4 || indices.size() != 6) return identity;
    for (float value : clipBounds) if (!std::isfinite(value)) return identity;
    // An oversized scissor cannot extend the rasterizer's clip volume.
    for (unsigned axis = 0; axis < 2; ++axis) {
        clipBounds[axis] = (std::max)(-1.f, clipBounds[axis]);
        clipBounds[axis + 2] = (std::min)(1.f, clipBounds[axis + 2]);
    }
    if (!(clipBounds[2] > clipBounds[0]) || !(clipBounds[3] > clipBounds[1])) return identity;

    // Projection can introduce rounding noise, but not a perspective change.
    const auto same = [](double a, double b) {
        return std::abs(a - b) <= 1e-6 * (std::max)(std::abs(a), std::abs(b));
    };
    std::array<std::array<double, 2>, 4> screen{};
    for (std::size_t i = 0; i < clip.size(); ++i) {
        const auto& vertex = clip[i];
        for (float value : vertex) if (!std::isfinite(value)) return identity;
        if (!(vertex[3] > 0) || vertex[2] < 0 || vertex[2] > vertex[3] ||
            !same(vertex[3], clip[0][3]) || !same(vertex[2], clip[0][2])) return identity;
        screen[i] = {double(vertex[0]) / vertex[3], double(vertex[1]) / vertex[3]};
    }
    auto low = screen[0], high = screen[0];
    for (const auto& vertex : screen) for (unsigned axis = 0; axis < 2; ++axis) {
        low[axis] = (std::min)(low[axis], vertex[axis]);
        high[axis] = (std::max)(high[axis], vertex[axis]);
    }
    if (!(high[0] > low[0]) || !(high[1] > low[1]) ||
        low[0] < clipBounds[0] || low[1] < clipBounds[1] ||
        high[0] > clipBounds[2] || high[1] > clipBounds[3]) return identity;

    // Four distinct corners, rather than an arbitrary four-vertex polygon.
    std::array<unsigned, 4> corners{};
    unsigned seen = 0;
    for (std::size_t i = 0; i < screen.size(); ++i) {
        unsigned corner = 0;
        for (unsigned axis = 0; axis < 2; ++axis) {
            const double tolerance = 1e-6 * (std::max)(1.0,
                (std::max)(std::abs(low[axis]), std::abs(high[axis])));
            const bool atLow = std::abs(screen[i][axis] - low[axis]) <= tolerance;
            const bool atHigh = std::abs(screen[i][axis] - high[axis]) <= tolerance;
            if (atLow == atHigh) return identity;
            if (atHigh) corner |= 1u << axis;
        }
        if (seen & (1u << corner)) return identity;
        seen |= 1u << corner;
        corners[i] = corner;
    }
    if (seen != 15) return identity;
    for (uint16_t index : indices) if (index >= clip.size()) return identity;
    unsigned firstMask = 0, secondMask = 0;
    for (unsigned i = 0; i < 3; ++i) {
        firstMask |= 1u << indices[i];
        secondMask |= 1u << indices[i + 3];
    }
    const auto threeDistinct = [](unsigned mask) { return mask == 7 || mask == 11 || mask == 13 || mask == 14; };
    if (!threeDistinct(firstMask) || !threeDistinct(secondMask) ||
        (firstMask | secondMask) != 15) return identity;
    // The shared edge must be a diagonal. Sharing an outer edge can create
    // overlapping triangles even when their total area equals the rectangle.
    const unsigned shared = firstMask & secondMask;
    unsigned diagonal = 0;
    for (unsigned i = 0; i < 4; ++i) if (shared & (1u << i)) diagonal ^= corners[i];
    if (diagonal != 3) return identity;
    const auto area = [&](unsigned triangle) {
        const auto& a = screen[indices[triangle]], &b = screen[indices[triangle + 1]],
                  &c = screen[indices[triangle + 2]];
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    };
    const double firstArea = area(0), secondArea = area(3);
    if (!firstArea || !secondArea || (firstArea > 0) != (secondArea > 0)) return identity;

    const std::array<double, 2> center{(low[0] + high[0]) * .5, (low[1] + high[1]) * .5};
    const std::array<double, 2> half{(high[0] - low[0]) * .5, (high[1] - low[1]) * .5};
    double scale = 1.25;
    for (unsigned axis = 0; axis < 2; ++axis) {
        scale = (std::min)(scale, (center[axis] - clipBounds[axis]) / half[axis]);
        scale = (std::min)(scale, (clipBounds[axis + 2] - center[axis]) / half[axis]);
    }
    if (!(scale > 1 + 1e-6)) return identity;
    const std::array<float, 4> result{float(scale), float((1 - scale) * center[0]),
                                   float((1 - scale) * center[1]), 1};
    for (float value : result) if (!std::isfinite(value)) return identity;
    return result;
}
} // namespace DarkRecomp::Prompts
