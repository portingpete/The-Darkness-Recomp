#include "engine_transforms.h"
#include "simple_mesh.h"
#include <Windows.h>
#include <xmmintrin.h>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00;
std::atomic<uint64_t> attempts{0}, decoded{0}, compared{0}, equal{0};
struct FloatMode {
    unsigned previous = _mm_getcsr();
    FloatMode() { _mm_setcsr((previous & ~0x6000u) | 0x9FC0u); }
    ~FloatMode() { _mm_setcsr(previous); }
};
bool finite(const EngineVector& vector) {
    for (float v : vector) if (!std::isfinite(v)) return false;
    return true;
}
// The current baseline x64 AOT uses SIMDe's scalar DPPS fallback (sse4.1.h),
// accumulating reversed guest lanes from zero. A hardware DPPS pairwise
// reduction differs by an ULP for some inputs. Match the actual AOT oracle;
// this file uses /fp:strict and does not claim Xbox hardware rounding parity.
float dot4(const EngineVector& a, const EngineVector& b) {
    const float x = a[0] * b[0], y = a[1] * b[1], z = a[2] * b[2], w = a[3] * b[3];
    return (((0.0f + w) + z) + y) + x;
}
float dot3(const EngineVector& a, const EngineVector& b) {
    const float x = a[0] * b[0], y = a[1] * b[1], z = a[2] * b[2];
    return ((0.0f + z) + y) + x;
}
bool copy(uint8_t* base, uint32_t address, void* out, size_t size) {
    if (!base || !address || uint64_t(address) + size > 0x100000000ull) return false;
    return copyRenderMemory(base,address,out,size);
}
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void matrix(const std::array<uint8_t, 64>& bytes, EngineMatrix& out) {
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) out[r][c] = std::bit_cast<float>(be32(bytes.data() + r * 16 + c * 4));
}
}

bool buildEngineTransformConstants(const EngineTransformInput& input,
                                  EngineTransformConstants& output) noexcept {
    for (const auto& row : input.modelView) if (!finite(row)) return false;
    for (const auto& row : input.projection) if (!finite(row)) return false;
    FloatMode mode;
    EngineTransformConstants result;
    const auto& model = input.modelView;
    for (unsigned column = 0; column < 4; ++column) {
        EngineVector p;
        for (unsigned row = 0; row < 4; ++row) p[row] = input.projection[row][column];
        for (unsigned row = 0; row < 3; ++row) result.vectors[column][row] = dot4(model[row], p);
        // 82A47230 in the hash-verified original image is (0,0,0,1).
        result.vectors[column][3] = dot4(EngineVector{0, 0, 0, 1}, p);
    }
    for (unsigned column = 0; column < 3; ++column) {
        auto& vector = result.vectors[4 + column];
        for (unsigned row = 0; row < 4; ++row) vector[row] = model[row][column];
        const float lengthSquared = dot3(vector, vector);
        if (!(lengthSquared > 0) || !std::isfinite(lengthSquared)) return false;
        float inverse = 1.0f / lengthSquared;
        if (!std::isfinite(inverse)) return false;
        // Original vrefp followed by two separately rounded refinements.
        for (unsigned step = 0; step < 2; ++step) {
            const float error = -(lengthSquared * inverse - 1.0f);
            inverse = inverse * error + inverse;
        }
        result.vectors[7][column] = inverse * dot3(model[column], model[3]);
    }
    // 82A47280 masks away the original reciprocal-of-zero w lane.
    result.vectors[7][3] = 0;
    for (const auto& vector : result.vectors) if (!finite(vector)) return false;
    output = result;
    return true;
}

bool snapshotEngineTransforms(uint8_t* base, EngineTransformSnapshot& output) noexcept {
    ++attempts;
    std::array<uint8_t, 12> selection, selectionAfter;
    std::array<uint8_t, 4> deviceBytes, deviceAfter;
    std::array<uint8_t, 64> model, modelAfter, projection, projectionAfter;
    if (!copy(base, context + 8224, selection.data(), selection.size()) ||
        !copy(base, context + 15748, deviceBytes.data(), deviceBytes.size())) return false;
    const uint32_t matrixBase = be32(selection.data()), index = be32(selection.data() + 8);
    const uint64_t address = uint64_t(matrixBase) + uint64_t(index) * 656 + 16;
    const uint32_t device = be32(deviceBytes.data());
    // The original vector loads align down. Unsupported unaligned selections
    // must not silently acquire different host semantics.
    if (!matrixBase || index >= 256 || (address & 15) || address + 64 > 0x100000000ull ||
        !copy(base, uint32_t(address), model.data(), model.size()) ||
        !copy(base, context + 17088, projection.data(), projection.size())) return false;
    EngineTransformSnapshot result;
    result.matrixAddress = uint32_t(address); result.deviceAddress = device;
    matrix(model, result.input.modelView); matrix(projection, result.input.projection);
    if (!buildEngineTransformConstants(result.input, result.constants)) return false;
    std::array<uint8_t, 128> original;
    const bool haveOriginal = device && !(device & 15) && uint64_t(device) + 2048 <= 0x100000000ull &&
        copy(base, device + 1920, original.data(), original.size());
    if (!copy(base, context + 8224, selectionAfter.data(), selectionAfter.size()) || selection != selectionAfter ||
        !copy(base, context + 15748, deviceAfter.data(), deviceAfter.size()) || deviceBytes != deviceAfter ||
        !copy(base, uint32_t(address), modelAfter.data(), modelAfter.size()) || model != modelAfter ||
        !copy(base, context + 17088, projectionAfter.data(), projectionAfter.size()) || projection != projectionAfter)
        return false;
    if (haveOriginal) {
        bool match = true;
        for (unsigned row = 0; row < 8; ++row)
            for (unsigned c = 0; c < 4; ++c)
                match &= be32(original.data() + row * 16 + c * 4) == std::bit_cast<uint32_t>(result.constants.vectors[row][c]);
        result.originalComparison = match ? TransformComparison::equal : TransformComparison::different;
        ++compared; if (match) ++equal;
    }
    ++decoded;
    output = result;
    return true;
}

void printEngineTransformCounters() {
    std::fprintf(stderr, "[EngineTransforms] attempts=%llu decoded=%llu compared=%llu bitExact=%llu different=%llu (owned constants; not rendered world meshes)\n",
        attempts.load(), decoded.load(), compared.load(), equal.load(), compared.load() - equal.load());
}
}
