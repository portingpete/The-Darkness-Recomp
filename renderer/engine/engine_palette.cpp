#include "engine_palette.h"
#include "engine_performance.h"
#include "render_trace.h"
#include "simple_mesh.h"
#include "ppc_recomp_shared.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00;
std::atomic<uint64_t> attempts{0}, captured{0}, compared{0}, equal{0}, empty{0};
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool copy(uint8_t* base, uint64_t address, void* out, size_t size) {
    if (!base || !address || address + size > 0x100000000ull) return false;
    return copyRenderMemory(base,address,out,size);
}
bool readInput(uint8_t* base, uint32_t source, std::array<uint8_t, 12>& header, EnginePaletteInput& out) {
    if (!copy(base, source, header.data(), header.size())) return false;
    EnginePaletteInput result; result.requestedCount = be32(header.data() + 8);
    const auto count = std::min(result.requestedCount, 52u);
    const uint32_t matrices = be32(header.data()), indices = be32(header.data() + 4);
    if (count && (!matrices || (matrices & 3) || (indices & 1))) return false;
    std::array<uint8_t, 104> selection{}, afterSelection{};
    if (count && indices && !copy(base, indices, selection.data(), count * 2)) return false;
    for (unsigned m = 0; m < count; ++m) {
        const unsigned index = indices ? (unsigned(selection[m * 2]) << 8 | selection[m * 2 + 1]) : m;
        std::array<uint8_t, 64> bytes{};
        if (!copy(base, uint64_t(matrices) + index * 64, bytes.data(), bytes.size())) return false;
        for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
            result.selected[m][r][c] = be32(bytes.data() + r * 16 + c * 4);
    }
    std::array<uint8_t, 12> after{};
    if (!copy(base, source, after.data(), after.size()) || header != after ||
        (count && indices && (!copy(base, indices, afterSelection.data(), count * 2) || selection != afterSelection))) return false;
    out = result; return true;
}
}
bool buildEnginePaletteConstants(const EngineVertexDescriptor& initial,
                                  const EnginePaletteInput& input, EnginePaletteConstants& output) noexcept {
    EnginePaletteConstants result; result.descriptor = initial; result.descriptor.palette = 96;
    const auto count = std::min(input.requestedCount, 52u);
    for (unsigned m = 0; m < count; ++m) for (unsigned c = 0; c < 3; ++c) for (unsigned r = 0; r < 4; ++r) {
        const auto value = input.selected[m][r][c], exponent = value & 0x7F800000u;
        if (exponent == 0x7F800000u || (!exponent && (value & 0x007FFFFFu))) return false;
        result.vectors[m * 3 + c][r] = value;
    }
    result.vectorCount = count * 3; output = result; return true;
}
bool beginEnginePaletteObservation(uint8_t* base, uint32_t descriptor, uint32_t cursor,
                                   uint32_t source, EnginePaletteObservation& output) noexcept {
    ++attempts;
    EnginePaletteObservation result; result.descriptorAddress = descriptor; result.cursorAddress = cursor; result.sourceAddress = source;
    std::array<uint8_t, 80> bytes{}, afterDescriptor{};
    std::array<uint8_t, 12> afterHeader{}; EnginePaletteInput afterInput;
    if (!copy(base, descriptor, bytes.data(), bytes.size()) || !readInput(base, source, result.sourceHeader, result.input) ||
        !buildEnginePaletteConstants(decodeEngineVertexDescriptor(bytes), result.input, result.constants)) return false;
    if (result.constants.vectorCount) {
        std::array<uint8_t, 4> cursorBytes{}, device{};
        if (!copy(base, cursor, cursorBytes.data(), 4) || !copy(base, context + 15748, device.data(), 4)) return false;
        result.deviceAddress = be32(device.data());
        if (!result.deviceAddress || (result.deviceAddress & 15) ||
            uint64_t(result.deviceAddress) + 3456 + result.constants.vectorCount * 16 > 0x100000000ull) return false;
    }
    if (!readInput(base, source, afterHeader, afterInput) || result.sourceHeader != afterHeader || result.input != afterInput ||
        !copy(base, descriptor, afterDescriptor.data(), afterDescriptor.size()) || bytes != afterDescriptor) return false;
    ++captured; output = result; return true;
}
void finishEnginePaletteObservation(uint8_t* base, uint32_t returnedVectors, EnginePaletteObservation& observation) noexcept {
    observation.comparison = TransformComparison::unavailable;
    std::array<uint8_t, 80> descriptor{}; std::array<uint8_t, 12> header{}; EnginePaletteInput input;
    if (!readInput(base, observation.sourceAddress, header, input) || observation.sourceHeader != header || input != observation.input ||
        !copy(base, observation.descriptorAddress, descriptor.data(), descriptor.size())) return;
    const auto count = observation.constants.vectorCount;
    bool match = returnedVectors == count && descriptor == encodeEngineVertexDescriptor(observation.constants.descriptor);
    if (count) {
        std::array<uint8_t, 4> device{}, cursor{}; std::array<uint8_t, 2496> words{};
        if (!copy(base, context + 15748, device.data(), 4) || be32(device.data()) != observation.deviceAddress ||
            !copy(base, observation.cursorAddress, cursor.data(), 4) ||
            !copy(base, uint64_t(observation.deviceAddress) + 3456, words.data(), count * 16)) return;
        match &= be32(cursor.data()) == observation.deviceAddress + 3456 + count * 16;
        for (unsigned v = 0; v < count; ++v) for (unsigned c = 0; c < 4; ++c)
            match &= be32(words.data() + v * 16 + c * 4) == observation.constants.vectors[v][c];
    } else ++empty;
    ++compared; if (match) ++equal;
    observation.comparison = match ? TransformComparison::equal : TransformComparison::different;
}
void printEnginePaletteCounters() {
    const auto matches = equal.load(), comparisons = compared.load();
    std::fprintf(stderr, "[EnginePalette] attempts=%llu captured=%llu compared=%llu bitExact=%llu different=%llu empty=%llu (owned matrix data; not rendered world meshes)\n",
        attempts.load(), captured.load(), comparisons, matches, comparisons - matches, empty.load());
}
}
extern "C" PPC_FUNC(__imp__sub_8224AB18);
PPC_FUNC(sub_8224AB18) {
    DarkRecomp::Native::EngineCpuScope profile(DarkRecomp::Native::EnginePhase::palette);
    using namespace DarkRecomp::Native;
    EnginePaletteObservation observation;
    const bool observe = uint32_t(ctx.lr) == 0x82249110 && renderTraceEnabled() &&
        beginEnginePaletteObservation(base, ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, observation);
    __imp__sub_8224AB18(ctx, base);
    if (observe) finishEnginePaletteObservation(base, ctx.r3.u32, observation);
}
