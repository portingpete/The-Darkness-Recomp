#pragma once
#include "engine_transforms.h"
#include <optional>

namespace DarkRecomp::Native {
using EngineConstantWords = std::array<uint32_t, 4>;
struct EngineTextureInput {
    bool enabled = false, hasParameters = false;
    uint32_t componentMask = 0;
    std::array<uint8_t, 8> modes{};
    std::array<std::optional<EngineMatrix>, 8> matrices{};
    // Some modes copy uninterpreted words. Preserve their bits without
    // assigning unproven float, texture-generation or skinning semantics.
    std::array<EngineConstantWords, 80> parameters{};
    uint32_t parameterCount = 0;
    bool operator==(const EngineTextureInput&) const = default;
};
struct EngineTextureStageConstants {
    uint8_t mode = 0;
    std::optional<EngineMatrix> matrixColumns;
    std::array<EngineConstantWords, 10> parameters{};
    uint8_t parameterCount = 0;
    // Original engine descriptor references, for comparison only. These
    // numbers are not a GPU register file or a native binding contract.
    uint8_t matrixReference = 0, parameterReference = 0;
};
struct EngineTextureConstants {
    std::array<EngineTextureStageConstants, 8> stages{};
    uint32_t vectorCount = 0, parametersConsumed = 0;
};

// Reconstruct original 8224A2E8. No guest execution or FP-state changes.
// Unsupported modes, nonfinite matrices and short data fail unchanged.
bool buildEngineTextureConstants(const EngineTextureInput& input,
                                 EngineTextureConstants& output) noexcept;

struct EngineTextureObservation {
    uint32_t descriptorAddress = 0, cursorAddress = 0, matricesAddress = 0, attributesAddress = 0;
    uint32_t deviceAddress = 0, initialCursor = 0;
    std::array<uint8_t, 80> descriptor{};
    EngineTextureInput input{};
    EngineTextureConstants constants{};
    TransformComparison comparison = TransformComparison::unavailable;
};
// Observe the engine preparation call, then compare its actual output. No
// guest registers/memory are changed. Observations do not submit world meshes.
bool beginEngineTextureObservation(uint8_t* base, uint32_t descriptor, uint32_t cursor,
                                   uint32_t matrices, uint32_t attributes,
                                   EngineTextureObservation& output) noexcept;
void finishEngineTextureObservation(uint8_t* base, uint32_t returnedVectors,
                                    EngineTextureObservation& observation) noexcept;
void printEngineTextureCounters();
}
