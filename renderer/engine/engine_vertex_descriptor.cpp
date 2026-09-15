#include "engine_vertex_descriptor.h"
#include "simple_mesh.h"
#include <Windows.h>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t context = 0x82A69B00, modeFlags = 0x82A5CD88, modeReservations = 0x82A5CDF4;
constexpr auto makeModes() {
    std::array<EngineVertexMode, 27> result{};
    constexpr uint8_t counts[]{0,4,3,10,0,0,0,1,0,1,3,0,0,0,1,2,2,6,8,1,1,3,2,0,0,0,4};
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i].reservation = counts[i];
        if (i >= 11) result[i].flags |= 0x00200000;
        if (i >= 18) result[i].flags |= 0x00400000;
        if (i == 0 || i == 7 || i == 13 || (i >= 18 && i <= 24)) result[i].flags |= 0x40000000;
    }
    return result;
}
constexpr auto modes = makeModes();
struct Counters {
    std::atomic<uint64_t> attempts{0}, captured{0}, compared{0}, equal{0}, empty{0};
} descriptorStats, conversionStats;
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
void put32(uint8_t* p, uint32_t value) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> ((3 - i) * 8)); }
void put16(uint8_t* p, uint16_t value) { p[0] = uint8_t(value >> 8); p[1] = uint8_t(value); }
bool copy(uint8_t* base, uint32_t address, void* out, size_t size) {
    if (!base || !address || uint64_t(address) + size > 0x100000000ull) return false;
    return copyRenderMemory(base,address,out,size);
}
bool initializedModes(uint8_t* base) {
    std::array<uint8_t, 108> flags{}; std::array<uint8_t, 27> reservations{};
    if (!copy(base, modeFlags, flags.data(), flags.size()) ||
        !copy(base, modeReservations, reservations.data(), reservations.size())) return false;
    for (unsigned i = 0; i < modes.size(); ++i)
        if (be32(flags.data() + i * 4) != modes[i].flags || reservations[i] != modes[i].reservation) return false;
    return true;
}
bool readDescriptorInput(uint8_t* base, uint32_t attributes, uint32_t matrices,
                         uint32_t enabledCoordinates, EngineVertexDescriptorInput& result) {
    std::array<uint8_t, 96> bytes{}; std::array<uint8_t, 32> pointers{};
    if (!copy(base, attributes, bytes.data(), bytes.size()) || !copy(base, matrices, pointers.data(), pointers.size())) return false;
    result = {}; result.enabledCoordinates = enabledCoordinates; result.materialFlags = be32(bytes.data() + 92);
    std::memcpy(result.modes.data(), bytes.data() + 48, 8);
    for (unsigned s = 0; s < 8; ++s) if (be32(pointers.data() + s * 4)) result.matrixMask |= 1u << s;
    return true;
}
uint32_t conversionVectors(const EngineVertexDescriptor& descriptor) {
    return 2 * (std::popcount(descriptor.flags & 255u) + unsigned(bool(descriptor.flags & 0x04000000)));
}
bool readWords(uint8_t* base, uint32_t address, uint32_t count, std::array<EngineConstantWords, 18>& words) {
    words = {};
    if (!count) return true;
    std::array<uint8_t, 288> bytes{};
    if (count > 18 || (address & 15) || !copy(base, address, bytes.data(), count * 16)) return false;
    for (unsigned v = 0; v < count; ++v) for (unsigned c = 0; c < 4; ++c)
        words[v][c] = be32(bytes.data() + v * 16 + c * 4);
    return true;
}
void record(Counters& stats, TransformComparison& comparison, bool match) {
    ++stats.compared; if (match) ++stats.equal;
    comparison = match ? TransformComparison::equal : TransformComparison::different;
}
}

const std::array<EngineVertexMode, 27>& engineVertexModes() noexcept { return modes; }
EngineVertexDescriptor decodeEngineVertexDescriptor(const std::array<uint8_t, 80>& b) noexcept {
    EngineVertexDescriptor d;
    d.coordinateMapping = be32(b.data()); d.flags = be32(b.data() + 4);
    std::memcpy(d.modes.data(), b.data() + 8, 8);
    d.declarationFlags = be32(b.data() + 16); d.textureReservation = be16(b.data() + 20); d.reserved22 = be16(b.data() + 22);
    d.conversionBase = b[24]; d.positionConversion = b[25]; d.palette = b[26]; d.color = b[27];
    std::memcpy(d.parameters.data(), b.data() + 28, 32); std::memcpy(d.matrices.data(), b.data() + 60, 8);
    std::memcpy(d.conversions.data(), b.data() + 68, 8); d.reserved76 = be32(b.data() + 76);
    return d;
}
std::array<uint8_t, 80> encodeEngineVertexDescriptor(const EngineVertexDescriptor& d) noexcept {
    std::array<uint8_t, 80> b{};
    put32(b.data(), d.coordinateMapping); put32(b.data() + 4, d.flags);
    std::memcpy(b.data() + 8, d.modes.data(), 8); put32(b.data() + 16, d.declarationFlags);
    put16(b.data() + 20, d.textureReservation); put16(b.data() + 22, d.reserved22);
    b[24] = d.conversionBase; b[25] = d.positionConversion; b[26] = d.palette; b[27] = d.color;
    std::memcpy(b.data() + 28, d.parameters.data(), 32); std::memcpy(b.data() + 60, d.matrices.data(), 8);
    std::memcpy(b.data() + 68, d.conversions.data(), 8); put32(b.data() + 76, d.reserved76);
    return b;
}
bool buildEngineVertexDescriptor(const EngineVertexDescriptor& initial, const EngineVertexDescriptorInput& input,
                                 EngineVertexDescriptor& output) noexcept {
    auto result = initial;
    result.textureReservation = 0;
    for (unsigned s = 0; s < 8; ++s) {
        const unsigned mode = !input.modes[s] && !(input.enabledCoordinates & (1u << s)) ? 4 : input.modes[s];
        if (mode >= modes.size()) return false;
        result.modes[s] = uint8_t(mode); result.flags |= modes[mode].flags;
        result.textureReservation += modes[mode].reservation + ((input.matrixMask & (1u << s)) ? 4 : 0);
    }
    if (input.materialFlags & 0x8000) result.flags |= 0x00800000;
    output = result;
    return true;
}
bool buildEngineConversionConstants(const EngineVertexDescriptor& initial,
                                    const std::array<EngineConstantWords, 18>& words,
                                    uint32_t availableVectors, EngineConversionConstants& output) noexcept {
    const uint32_t required = conversionVectors(initial);
    if (availableVectors > 18 || availableVectors < required) return false;
    EngineConversionConstants result; result.descriptor = initial; result.vectorCount = required;
    unsigned vector = 0;
    if (initial.flags & 0x04000000) {
        result.position = EngineConversionPair{words[0], words[1]}; vector = 2;
        result.descriptor.positionConversion = 76;
    }
    for (unsigned s = 0; s < 8; ++s) if (initial.flags & (1u << s)) {
        result.coordinates[s] = EngineConversionPair{words[vector], words[vector + 1]};
        // Original A0A8 repeats the SAME comparison before all eight writes.
        // Preserve this behavior; do not invent a per-destination remapping.
        if (((initial.coordinateMapping >> (8 + s * 3)) & 7u) == s)
            result.descriptor.conversions.fill(uint8_t(76 + vector));
        vector += 2;
    }
    output = result;
    return true;
}

bool beginEngineDescriptorObservation(uint8_t* base, uint32_t descriptor, uint32_t attributes,
                                      uint32_t matrices, uint32_t enabledCoordinates, EngineDescriptorObservation& output) noexcept {
    ++descriptorStats.attempts;
    EngineDescriptorObservation result; result.descriptorAddress = descriptor;
    result.attributesAddress = attributes; result.matricesAddress = matrices;
    std::array<uint8_t, 80> bytes{}, after{}; EngineVertexDescriptorInput inputAfter;
    if (!copy(base, descriptor, bytes.data(), bytes.size()) || !initializedModes(base) ||
        !readDescriptorInput(base, attributes, matrices, enabledCoordinates, result.input) ||
        !buildEngineVertexDescriptor(decodeEngineVertexDescriptor(bytes), result.input, result.expected) ||
        !readDescriptorInput(base, attributes, matrices, enabledCoordinates, inputAfter) || inputAfter != result.input ||
        !copy(base, descriptor, after.data(), after.size()) || bytes != after) return false;
    ++descriptorStats.captured; output = result; return true;
}
void finishEngineDescriptorObservation(uint8_t* base, EngineDescriptorObservation& observation) noexcept {
    observation.comparison = TransformComparison::unavailable;
    EngineVertexDescriptorInput after; std::array<uint8_t, 80> bytes{};
    if (!initializedModes(base) || !readDescriptorInput(base, observation.attributesAddress, observation.matricesAddress,
        observation.input.enabledCoordinates, after) || after != observation.input ||
        !copy(base, observation.descriptorAddress, bytes.data(), bytes.size())) return;
    if (!observation.expected.textureReservation) ++descriptorStats.empty;
    record(descriptorStats, observation.comparison, bytes == encodeEngineVertexDescriptor(observation.expected));
}
bool beginEngineConversionObservation(uint8_t* base, uint32_t descriptor, uint32_t cursor,
                                      uint32_t source, EngineConversionObservation& output) noexcept {
    ++conversionStats.attempts;
    EngineConversionObservation result; result.descriptorAddress = descriptor; result.cursorAddress = cursor; result.sourceAddress = source;
    std::array<uint8_t, 80> bytes{}, descriptorAfter{}; std::array<uint8_t, 4> cursorBytes{}, deviceBytes{};
    if (!copy(base, descriptor, bytes.data(), bytes.size()) || !copy(base, cursor, cursorBytes.data(), 4)) return false;
    result.initialCursor = be32(cursorBytes.data());
    const auto initial = decodeEngineVertexDescriptor(bytes); const auto count = conversionVectors(initial);
    if (count) {
        if (!copy(base, context + 15748, deviceBytes.data(), 4)) return false;
        result.deviceAddress = be32(deviceBytes.data());
        if (!result.deviceAddress || (result.deviceAddress & 15) || uint64_t(result.deviceAddress) + 3136 + count * 16 > 0x100000000ull) return false;
    }
    std::array<EngineConstantWords, 18> sourceAfter{};
    if (!readWords(base, source, count, result.source) || !buildEngineConversionConstants(initial, result.source, count, result.constants) ||
        !readWords(base, source, count, sourceAfter) || sourceAfter != result.source ||
        !copy(base, descriptor, descriptorAfter.data(), descriptorAfter.size()) || bytes != descriptorAfter) return false;
    ++conversionStats.captured; output = result; return true;
}
void finishEngineConversionObservation(uint8_t* base, uint32_t returnedVectors, EngineConversionObservation& observation) noexcept {
    observation.comparison = TransformComparison::unavailable;
    std::array<uint8_t, 80> descriptor{}; std::array<uint8_t, 4> cursor{}, device{};
    const uint32_t count = observation.constants.vectorCount;
    std::array<EngineConstantWords, 18> source{}, original{};
    if (!copy(base, observation.descriptorAddress, descriptor.data(), descriptor.size()) ||
        !copy(base, observation.cursorAddress, cursor.data(), 4) || !readWords(base, observation.sourceAddress, count, source) || source != observation.source) return;
    if (count && (!copy(base, context + 15748, device.data(), 4) || be32(device.data()) != observation.deviceAddress ||
                  !readWords(base, observation.deviceAddress + 3136, count, original))) return;
    const uint32_t expectedCursor = count ? observation.deviceAddress + 3136 + count * 16 : observation.initialCursor;
    bool match = returnedVectors == count && be32(cursor.data()) == expectedCursor &&
                 descriptor == encodeEngineVertexDescriptor(observation.constants.descriptor);
    for (unsigned v = 0; v < count; ++v) match &= original[v] == observation.source[v];
    if (!count) ++conversionStats.empty;
    record(conversionStats, observation.comparison, match);
}
void printEngineVertexDescriptorCounters() {
    auto print = [](const char* name, const Counters& s) {
        std::fprintf(stderr, "[%s] attempts=%llu captured=%llu compared=%llu bitExact=%llu different=%llu empty=%llu (owned engine data; not rendered world meshes)\n",
            name, s.attempts.load(), s.captured.load(), s.compared.load(), s.equal.load(), s.compared.load() - s.equal.load(), s.empty.load());
    };
    print("EngineVertexDescriptor", descriptorStats); print("EngineConversionConstants", conversionStats);
}
}
