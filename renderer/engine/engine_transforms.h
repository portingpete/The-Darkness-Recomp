#pragma once
#include <array>
#include <cstdint>

namespace DarkRecomp::Native {
using EngineVector = std::array<float, 4>;
using EngineMatrix = std::array<EngineVector, 4>;
struct EngineTransformInput {
    EngineMatrix modelView{}, projection{}; // Original row-major engine data.
};
struct EngineTransformConstants {
    // Exact eight-vector result of engine routine 82248A78. Vectors 0..3
    // transpose the projection product with model row 3 replaced by (0,0,0,1).
    // Vectors 4..6 are model columns including translation. Vector 7 contains
    // the original dot/column-length-squared quotients, with w cleared.
    // This is NOT a generic inverse, nor a complete vertex shader/skinning contract.
    std::array<EngineVector, 8> vectors{};
};
enum class TransformComparison : uint8_t { unavailable, equal, different };
struct EngineTransformSnapshot {
    uint32_t matrixAddress = 0, deviceAddress = 0;
    EngineTransformInput input{};
    EngineTransformConstants constants{};
    TransformComparison originalComparison = TransformComparison::unavailable;
};

// Owned, host-only arithmetic. Matches the current original-AOT vector
// operation order under round-to-nearest with denormals flushed. No guest
// code is executed. Rejects nonfinite inputs/results and zero-length columns;
// failure leaves output unchanged and the caller's host FP state is restored.
bool buildEngineTransformConstants(const EngineTransformInput& input,
                                  EngineTransformConstants& output) noexcept;
// Read the selected engine model/projection and build owned constants. The
// optional comparison reads the original engine's CPU output, not GPU packets.
// Source pointers/data are rechecked; no guest memory or register is modified.
bool snapshotEngineTransforms(uint8_t* base, EngineTransformSnapshot& output) noexcept;
void printEngineTransformCounters();
}
