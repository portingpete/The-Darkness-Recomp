#pragma once
#include "engine_texture_constants.h"

namespace DarkRecomp::Native {
// Engine-owned 80-byte descriptor, not a GPU shader/register representation.
// Unknown fields are retained so updating one contract preserves the rest.
struct EngineVertexDescriptor {
    uint32_t coordinateMapping = 0xFAC68800, flags = 0;
    std::array<uint8_t, 8> modes{};
    uint32_t declarationFlags = 0;
    uint16_t textureReservation = 0, reserved22 = 0;
    uint8_t conversionBase = 76, positionConversion = 0, palette = 96, color = 10;
    std::array<std::array<uint8_t, 4>, 8> parameters{};
    std::array<uint8_t, 8> matrices{}, conversions{};
    uint32_t reserved76 = 0;
    bool operator==(const EngineVertexDescriptor&) const = default;
};
EngineVertexDescriptor decodeEngineVertexDescriptor(const std::array<uint8_t, 80>& bytes) noexcept;
std::array<uint8_t, 80> encodeEngineVertexDescriptor(const EngineVertexDescriptor& descriptor) noexcept;
struct EngineVertexMode {
    uint32_t flags = 0;
    uint8_t reservation = 0;
    bool operator==(const EngineVertexMode&) const = default;
};
// Original initialization 8223AFE8, including all 27 table entries. These
// reservation counts need not equal the vectors later written by 8224A2E8.
const std::array<EngineVertexMode, 27>& engineVertexModes() noexcept;
struct EngineVertexDescriptorInput {
    std::array<uint8_t, 8> modes{};
    uint32_t matrixMask = 0, enabledCoordinates = 0, materialFlags = 0;
    bool operator==(const EngineVertexDescriptorInput&) const = default;
};
bool buildEngineVertexDescriptor(const EngineVertexDescriptor& initial,
                                 const EngineVertexDescriptorInput& input,
                                 EngineVertexDescriptor& output) noexcept;
struct EngineConversionPair {
    EngineConstantWords scale{}, offset{};
    bool operator==(const EngineConversionPair&) const = default;
};
struct EngineConversionConstants {
    EngineVertexDescriptor descriptor;
    std::optional<EngineConversionPair> position;
    std::array<std::optional<EngineConversionPair>, 8> coordinates{};
    uint32_t vectorCount = 0;
    bool operator==(const EngineConversionConstants&) const = default;
};
// Original 8224A0A8: packed scale/offset pairs and exact descriptor writes.
// Failure preserves output. No guest execution or FP arithmetic occurs.
bool buildEngineConversionConstants(const EngineVertexDescriptor& initial,
                                    const std::array<EngineConstantWords, 18>& words,
                                    uint32_t availableVectors, EngineConversionConstants& output) noexcept;

struct EngineDescriptorObservation {
    uint32_t descriptorAddress = 0, attributesAddress = 0, matricesAddress = 0;
    EngineVertexDescriptorInput input{};
    EngineVertexDescriptor expected;
    TransformComparison comparison = TransformComparison::unavailable;
};
struct EngineConversionObservation {
    uint32_t descriptorAddress = 0, cursorAddress = 0, sourceAddress = 0, deviceAddress = 0, initialCursor = 0;
    std::array<EngineConstantWords, 18> source{};
    EngineConversionConstants constants;
    TransformComparison comparison = TransformComparison::unavailable;
};
bool beginEngineDescriptorObservation(uint8_t* base, uint32_t descriptor, uint32_t attributes,
                                      uint32_t matrices, uint32_t enabledCoordinates,
                                      EngineDescriptorObservation& output) noexcept;
void finishEngineDescriptorObservation(uint8_t* base, EngineDescriptorObservation& observation) noexcept;
bool beginEngineConversionObservation(uint8_t* base, uint32_t descriptor, uint32_t cursor,
                                      uint32_t source, EngineConversionObservation& output) noexcept;
void finishEngineConversionObservation(uint8_t* base, uint32_t returnedVectors,
                                       EngineConversionObservation& observation) noexcept;
void printEngineVertexDescriptorCounters();
}
