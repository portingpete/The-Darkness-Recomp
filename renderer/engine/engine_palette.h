#pragma once
#include "engine_vertex_descriptor.h"

namespace DarkRecomp::Native {
using EngineMatrixWords = std::array<EngineConstantWords, 4>;
struct EnginePaletteInput {
    uint32_t requestedCount = 0;
    // Owned matrices after the original contiguous/indexed selection.
    std::array<EngineMatrixWords, 52> selected{};
    bool operator==(const EnginePaletteInput&) const = default;
};
struct EnginePaletteConstants {
    EngineVertexDescriptor descriptor;
    std::array<EngineConstantWords, 156> vectors{};
    uint32_t vectorCount = 0;
    bool operator==(const EnginePaletteConstants&) const = default;
};
// Original 8224AB18: clamp to52 matrices, copy the first three columns and
// set descriptor reference96 even for zero matrices. No shader/weight semantics
// are inferred. Selected nonfinite/subnormal components are unsupported; normal
// values and signed zero are copied bit-exactly without host FP arithmetic.
// The unused fourth column is ignored. Failure preserves output.
bool buildEnginePaletteConstants(const EngineVertexDescriptor& initial,
                                  const EnginePaletteInput& input, EnginePaletteConstants& output) noexcept;
struct EnginePaletteObservation {
    uint32_t descriptorAddress = 0, cursorAddress = 0, sourceAddress = 0, deviceAddress = 0;
    std::array<uint8_t, 12> sourceHeader{};
    EnginePaletteInput input;
    EnginePaletteConstants constants;
    TransformComparison comparison = TransformComparison::unavailable;
};
bool beginEnginePaletteObservation(uint8_t* base, uint32_t descriptor, uint32_t cursor,
                                   uint32_t source, EnginePaletteObservation& output) noexcept;
void finishEnginePaletteObservation(uint8_t* base, uint32_t returnedVectors,
                                    EnginePaletteObservation& observation) noexcept;
void printEnginePaletteCounters();
}
