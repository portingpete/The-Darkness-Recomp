#pragma once
#include "renderer/engine/prompt_layout.h"
#include <cstdio>
#include <limits>

static void testPromptLayout() {
    using DarkRecomp::Prompts::promptQuadTransform;
    using Quad = std::array<std::array<float, 4>, 4>;
    constexpr std::array<uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    constexpr std::array<float, 4> identity{1, 0, 0, 0};
    const auto quad = [](float cx, float cy, float hx, float hy) -> Quad {
        return {{{cx - hx, cy - hy, .5f, 1}, {cx + hx, cy - hy, .5f, 1},
                 {cx + hx, cy + hy, .5f, 1}, {cx - hx, cy + hy, .5f, 1}}};
    };
    const auto layoutNear = [](double actual, double expected) {
        require(std::abs(actual - expected) < 1e-6, "Prompt layout numerical result differs");
    };
    auto square = quad(0, 0, .5f, .5f);
    const auto snapshot = square;
    const auto transformed = promptQuadTransform(square, indices);
    require(transformed[3] == 1, "Intact prompt quad was not enlarged");
    layoutNear(transformed[0], 1.25); layoutNear(transformed[1], 0); layoutNear(transformed[2], 0);
    // A 32px square in a 64px viewport must become 40px at the same center.
    const double left = (transformed[0] * square[0][0] + transformed[1] + 1) * 32;
    const double right = (transformed[0] * square[1][0] + transformed[1] + 1) * 32;
    layoutNear(left, 12); layoutNear(right, 52); layoutNear(right - left, 40);
    require(square == snapshot, "Prompt layout changed its input geometry");

    auto offset = quad(.25f, -.25f, .25f, .25f);
    const auto offsetResult = promptQuadTransform(offset, indices);
    layoutNear(offsetResult[0], 1.25); layoutNear(offsetResult[1], -.0625); layoutNear(offsetResult[2], .0625);
    layoutNear(offsetResult[0] * .25 + offsetResult[1], .25);
    layoutNear(offsetResult[0] * -.25 + offsetResult[2], -.25);
    auto homogeneous = offset;
    for (auto& vertex : homogeneous) for (float& value : vertex) value *= 2;
    require(promptQuadTransform(homogeneous, indices) == offsetResult,
            "Uniform clip W changed prompt geometry in screen space");
    constexpr std::array<uint16_t, 6> reversed{0, 2, 1, 0, 3, 2};
    require(promptQuadTransform(square, reversed) == transformed, "Valid reverse winding was rejected");
    auto reordered = square; std::swap(reordered[1], reordered[3]);
    require(promptQuadTransform(reordered, indices) == transformed, "Corner storage order changed prompt size");

    const auto edge = promptQuadTransform(quad(.7f, 0, .25f, .25f), indices);
    layoutNear(edge[0], 1.2); layoutNear(edge[0] * .95f + edge[1], 1);
    const auto scissor = promptQuadTransform(square, indices, {-.6f, -.6f, .6f, .6f});
    layoutNear(scissor[0], 1.2); layoutNear(scissor[1], 0); layoutNear(scissor[2], 0);
    require(promptQuadTransform(quad(.75f, 0, .25f, .25f), indices) == identity,
            "Prompt touching the viewport edge was clipped by growth");
    require(promptQuadTransform(quad(.75f, 0, .25f, .25f), indices, {-2, -2, 2, 2}) == identity,
            "Oversized scissor let prompt growth exceed the clip volume");
    require(promptQuadTransform(quad(.9f, 0, .25f, .25f), indices) == identity,
            "Already cropped prompt was enlarged");
    require(promptQuadTransform(square, indices, {-.4f, -.6f, .6f, .6f}) == identity,
            "Already scissored prompt was enlarged");

    auto invalid = square; invalid[0] = invalid[1];
    require(promptQuadTransform(invalid, indices) == identity, "Duplicate corner was enlarged");
    invalid = square; invalid[0][0] = -.4f;
    require(promptQuadTransform(invalid, indices) == identity, "Trapezoid was enlarged");
    invalid = {{{0, -.5f, .5f, 1}, {.5f, 0, .5f, 1},
                {0, .5f, .5f, 1}, {-.5f, 0, .5f, 1}}};
    require(promptQuadTransform(invalid, indices) == identity, "Rotated quad was enlarged");
    invalid = square; invalid[0][3] = 1.01f;
    require(promptQuadTransform(invalid, indices) == identity, "Perspective quad was enlarged");
    invalid = square; invalid[0][2] = .6f;
    require(promptQuadTransform(invalid, indices) == identity, "Varying depth quad was enlarged");
    for (float depth : {-1.f, 2.f}) {
        invalid = square; for (auto& vertex : invalid) vertex[2] = depth;
        require(promptQuadTransform(invalid, indices) == identity, "Depth-clipped quad was enlarged");
    }
    for (float w : {0.f, -1.f}) {
        invalid = square; for (auto& vertex : invalid) vertex[3] = w;
        require(promptQuadTransform(invalid, indices) == identity, "Nonpositive clip W was enlarged");
    }
    invalid = square; for (auto& vertex : invalid) vertex[2] = 0;
    require(promptQuadTransform(invalid, indices) == transformed, "Valid near-plane HUD was rejected");
    invalid = square; for (auto& vertex : invalid) vertex[2] = 1;
    require(promptQuadTransform(invalid, indices) == transformed, "Valid far-plane HUD was rejected");

    for (auto bad : {std::array<uint16_t, 6>{0, 1, 4, 0, 2, 3},
                     std::array<uint16_t, 6>{0, 1, 1, 0, 2, 3},
                     std::array<uint16_t, 6>{0, 1, 2, 0, 3, 2},
                     std::array<uint16_t, 6>{0, 1, 2, 0, 1, 3},
                     std::array<uint16_t, 6>{0, 1, 2, 0, 1, 2}})
        require(promptQuadTransform(square, bad) == identity, "Invalid triangle topology was enlarged");
    require(promptQuadTransform(std::span(square).first(3), indices) == identity,
            "Incomplete quad was enlarged");
    require(promptQuadTransform(square, std::span(indices).first(5)) == identity,
            "Incomplete triangle list was enlarged");
    require(promptQuadTransform(quad(0, 0, 0, .5f), indices) == identity, "Zero-area quad was enlarged");
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            invalid = square; invalid[0][lane] = bad;
            require(promptQuadTransform(invalid, indices) == identity, "Nonfinite quad was enlarged");
        }
        require(promptQuadTransform(square, indices, {bad, -1, 1, 1}) == identity,
                "Nonfinite clip bounds were accepted");
    }
    require(promptQuadTransform(square, indices, {1, -1, -1, 1}) == identity,
            "Reversed clip bounds were accepted");
    std::puts("PromptLayout passed: 25% size, stable center, homogeneous projection, edge/scissor caps and strict invalid geometry rejection.");
}
