#include "engine_texture_constants.h"
#include "simple_mesh.h"
#include <Windows.h>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00;
constexpr uint32_t one = 0x3F800000, small = 0x3B800000;
std::atomic<uint64_t> attempts{0}, captured{0}, compared{0}, equal{0}, empty{0};
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool copy(uint8_t* base, uint32_t address, void* out, size_t size) {
    if (!base || !address || uint64_t(address) + size > 0x100000000ull) return false;
    return copyRenderMemory(base,address,out,size);
}
uint32_t parameterVectors(uint8_t mode) {
    switch (mode) {
    case 1: return 4;
    case 2: case 10: case 21: return 3;
    case 3: return 10;
    case 7: case 9: case 14: case 20: return 1;
    case 15: case 16: case 19: case 22: return 2;
    case 17: return 6;
    case 18: return 8;
    default: return 0;
    }
}
uint32_t sourceVectors(const EngineTextureInput& input) {
    if (!input.enabled || !input.hasParameters) return 0;
    uint32_t count = 0;
    for (unsigned s = 0; s < 8; ++s)
        count += input.modes[s] == 1 ? std::popcount((input.componentMask >> (s * 4)) & 15u) : parameterVectors(input.modes[s]);
    return count;
}
bool finiteBits(float v) { return (std::bit_cast<uint32_t>(v) & 0x7F800000u) != 0x7F800000u; }
bool sameBits(const EngineTextureInput& a, const EngineTextureInput& b) {
    // Float equality loses signed zero and can touch FP status for NaNs.
    if (a.enabled != b.enabled || a.hasParameters != b.hasParameters || a.componentMask != b.componentMask ||
        a.modes != b.modes || a.parameterCount != b.parameterCount || a.parameters != b.parameters) return false;
    for (unsigned s = 0; s < 8; ++s) {
        if (a.matrices[s].has_value() != b.matrices[s].has_value()) return false;
        if (a.matrices[s] && std::memcmp(&*a.matrices[s], &*b.matrices[s], sizeof(EngineMatrix))) return false;
    }
    return true;
}
// Read each compact selector twice around its pointees. The caller repeats
// the complete owned read as well to reject changing parameter/matrix bytes.
bool readInput(uint8_t* base, bool enabled, uint32_t matrices, uint32_t attributes, EngineTextureInput& out) {
    EngineTextureInput result; result.enabled = enabled;
    if (!enabled) { out = result; return true; }
    std::array<uint8_t, 36> attrs{}, afterAttrs{}; // +44 mask, +48 modes, +76 pointer
    std::array<uint8_t, 32> pointers{}, afterPointers{};
    if (uint64_t(attributes) + 80 > 0x100000000ull || !attributes ||
        !copy(base, attributes + 44, attrs.data(), attrs.size()) ||
        !copy(base, matrices, pointers.data(), pointers.size())) return false;
    result.componentMask = be32(attrs.data());
    std::memcpy(result.modes.data(), attrs.data() + 4, 8);
    const uint32_t parameters = be32(attrs.data() + 32);
    result.hasParameters = parameters != 0;
    for (unsigned s = 0; s < 8; ++s) {
        if (result.modes[s] > 22) return false;
        const uint32_t address = be32(pointers.data() + s * 4);
        if (!address) continue;
        // Matrix lfs reads are word based; vector parameter loads align down.
        std::array<uint8_t, 64> bytes{};
        if ((address & 3) || !copy(base, address, bytes.data(), bytes.size())) return false;
        EngineMatrix matrix{};
        for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
            matrix[r][c] = std::bit_cast<float>(be32(bytes.data() + r * 16 + c * 4));
        result.matrices[s] = matrix;
    }
    result.parameterCount = sourceVectors(result);
    if (result.parameterCount > result.parameters.size()) return false;
    if (result.parameterCount) {
        std::array<uint8_t, 1280> bytes{};
        if ((parameters & 15) || !copy(base, parameters, bytes.data(), result.parameterCount * 16)) return false;
        for (unsigned v = 0; v < result.parameterCount; ++v) for (unsigned c = 0; c < 4; ++c)
            result.parameters[v][c] = be32(bytes.data() + v * 16 + c * 4);
    }
    if (!copy(base, attributes + 44, afterAttrs.data(), afterAttrs.size()) || attrs != afterAttrs ||
        !copy(base, matrices, afterPointers.data(), afterPointers.size()) || pointers != afterPointers) return false;
    out = result;
    return true;
}
}

bool buildEngineTextureConstants(const EngineTextureInput& input, EngineTextureConstants& output) noexcept {
    EngineTextureConstants result;
    if (!input.enabled) { output = result; return true; }
    if (input.parameterCount > input.parameters.size() || sourceVectors(input) > input.parameterCount) return false;
    for (unsigned s = 0; s < 8; ++s) {
        auto& stage = result.stages[s];
        stage.mode = input.modes[s];
        if (stage.mode > 22) return false;
        if (input.matrices[s]) {
            stage.matrixColumns.emplace();
            for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
                const float v = (*input.matrices[s])[r][c];
                if (!finiteBits(v)) return false;
                (*stage.matrixColumns)[c][r] = v;
            }
            stage.matrixReference = uint8_t(12 + result.vectorCount);
            result.vectorCount += 4;
        }
        stage.parameterCount = uint8_t(parameterVectors(stage.mode));
        if (!stage.parameterCount) continue;
        stage.parameterReference = uint8_t(12 + result.vectorCount);
        result.vectorCount += stage.parameterCount;
        // Constants from original 8209DCBC (0), 8205C06C (1) and
        // 8209DE94 (1/256). The game image is verified by the oracle test.
        if (stage.mode == 1 || stage.mode == 20) stage.parameters[stage.parameterCount - 1][3] = one;
        if (stage.mode == 15 || stage.mode == 16) {
            stage.parameters[0][3] = small;
            stage.parameters[1] = {one, one, one, 0};
        }
        if (stage.mode == 10 || stage.mode == 21)
            for (unsigned i = 0; i < 3; ++i) stage.parameters[i][i] = one;
        if (stage.mode == 22) stage.parameters[1] = {one, one, 0, 0};
        if (input.hasParameters) for (unsigned v = 0; v < stage.parameterCount; ++v) {
            if (stage.mode == 1 && !(input.componentMask & (1u << (s * 4 + v)))) continue;
            stage.parameters[v] = input.parameters[result.parametersConsumed++];
        }
    }
    output = result;
    return true;
}

bool beginEngineTextureObservation(uint8_t* base, uint32_t descriptor, uint32_t cursor,
                                   uint32_t matrices, uint32_t attributes, EngineTextureObservation& output) noexcept {
    ++attempts;
    EngineTextureObservation result;
    result.descriptorAddress = descriptor; result.cursorAddress = cursor;
    result.matricesAddress = matrices; result.attributesAddress = attributes;
    std::array<uint8_t, 4> cursorBytes{}, deviceBytes{};
    if (!copy(base, descriptor, result.descriptor.data(), result.descriptor.size()) ||
        !copy(base, cursor, cursorBytes.data(), 4) || !copy(base, context + 15748, deviceBytes.data(), 4)) return false;
    result.initialCursor = be32(cursorBytes.data()); result.deviceAddress = be32(deviceBytes.data());
    const bool enabled = result.descriptor[20] || result.descriptor[21];
    if (enabled && (!result.deviceAddress || (result.deviceAddress & 15) ||
        uint64_t(result.deviceAddress) + 2112 + 112 * 16 > 0x100000000ull)) return false;
    EngineTextureInput again;
    if (!readInput(base, enabled, matrices, attributes, result.input) ||
        !buildEngineTextureConstants(result.input, result.constants) ||
        !readInput(base, enabled, matrices, attributes, again) || !sameBits(result.input, again)) return false;
    std::array<uint8_t, 80> descriptorAfter{};
    if (!copy(base, descriptor, descriptorAfter.data(), descriptorAfter.size()) || descriptorAfter != result.descriptor) return false;
    ++captured;
    output = result;
    return true;
}

void finishEngineTextureObservation(uint8_t* base, uint32_t returnedVectors, EngineTextureObservation& observation) noexcept {
    observation.comparison = TransformComparison::unavailable;
    EngineTextureInput after;
    std::array<uint8_t, 4> device{}, cursor{};
    std::array<uint8_t, 80> descriptor{};
    if (!readInput(base, observation.input.enabled, observation.matricesAddress, observation.attributesAddress, after) ||
        !sameBits(observation.input, after) || !copy(base, context + 15748, device.data(), 4) ||
        be32(device.data()) != observation.deviceAddress || !copy(base, observation.cursorAddress, cursor.data(), 4) ||
        !copy(base, observation.descriptorAddress, descriptor.data(), descriptor.size())) return;
    auto expectedDescriptor = observation.descriptor;
    const auto& constants = observation.constants;
    std::array<EngineConstantWords, 112> expected{};
    size_t count = 0;
    for (unsigned s = 0; s < 8; ++s) {
        const auto& stage = constants.stages[s];
        if (stage.matrixColumns) {
            expectedDescriptor[60 + s] = stage.matrixReference;
            for (const auto& column : *stage.matrixColumns) {
                for (unsigned c = 0; c < 4; ++c) expected[count][c] = std::bit_cast<uint32_t>(column[c]);
                ++count;
            }
        }
        if (stage.parameterCount) expectedDescriptor[28 + s * 4] = stage.parameterReference;
        for (unsigned v = 0; v < stage.parameterCount; ++v) expected[count++] = stage.parameters[v];
    }
    const uint32_t expectedCursor = observation.input.enabled ? observation.deviceAddress + 2112 + constants.vectorCount * 16 : observation.initialCursor;
    bool match = returnedVectors == constants.vectorCount && descriptor == expectedDescriptor && be32(cursor.data()) == expectedCursor;
    if (count) {
        std::array<uint8_t, 1792> original{};
        if (!copy(base, observation.deviceAddress + 2112, original.data(), count * 16)) return;
        for (unsigned v = 0; v < count; ++v) for (unsigned c = 0; c < 4; ++c)
            match &= be32(original.data() + v * 16 + c * 4) == expected[v][c];
    } else ++empty;
    ++compared; if (match) ++equal;
    observation.comparison = match ? TransformComparison::equal : TransformComparison::different;
}

void printEngineTextureCounters() {
    std::fprintf(stderr, "[EngineTextureConstants] attempts=%llu captured=%llu compared=%llu bitExact=%llu different=%llu empty=%llu (owned preparation data; not rendered world meshes)\n",
        attempts.load(), captured.load(), compared.load(), equal.load(), compared.load() - equal.load(), empty.load());
}
}
