#pragma once
#include "engine_vertex_descriptor.h"
#include <optional>

namespace DarkRecomp::Native {
using EngineVertexProgramKey = std::array<uint32_t, 6>;
struct EngineVertexProgramInput {
    uint32_t declarationAddress = 0, declarationFlags = 0, rendererFlags = 0;
    uint32_t enabledCoordinates = 0, conversionMask = 0, modelFlags = 0, materialFlags = 0;
    bool palette = false, secondBlendIndices = false;
    std::array<uint8_t, 8> modes{}, coordinateSources{};
    bool operator==(const EngineVertexProgramInput&) const = default;
};
// Original 82248C80 key: declaration identity followed by the first20 bytes
// of its completed descriptor. Selection is not native shader compilation.
bool buildEngineVertexProgramKey(const EngineVertexProgramInput& input, EngineVertexProgramKey& output) noexcept;
struct EngineVertexProgramNode {
    uint32_t address = 0; // Record start; tree links are at +40/+44.
    std::array<uint8_t, 48> bytes{};
    bool operator==(const EngineVertexProgramNode&) const = default;
};
struct EngineVertexProgramSelection {
    uint32_t recordAddress = 0, bindingAddress = 0, visitedCount = 0;
    std::array<EngineVertexProgramNode, 128> visited{};
};
// Bounded read-only original cache lookup. A valid miss succeeds with a zero
// record. Unreadable, overflowing or cyclic trees fail without changing output.
bool lookupEngineVertexProgram(uint8_t* base, uint32_t root, const EngineVertexProgramKey& key,
                               EngineVertexProgramSelection& output) noexcept;
struct EngineVertexProgramSource {
    EngineVertexProgramInput input;
    uint32_t matrixBase = 0, matrixIndex = 0, matrixAddress = 0, paletteAddress = 0, treeRoot = 0;
    bool operator==(const EngineVertexProgramSource&) const = default;
};
struct EngineVertexProgramObservation {
    EngineVertexProgramSource source;
    EngineVertexProgramKey key{};
    EngineVertexProgramSelection selection;
    uint32_t keyAddress = 0;
    TransformComparison comparison = TransformComparison::unavailable;
};
bool beginEngineVertexProgramObservation(uint8_t* base, uint32_t stack, EngineVertexProgramObservation& output) noexcept;
void finishEngineVertexProgramObservation(uint8_t* base, EngineVertexProgramObservation& observation) noexcept;
void printEngineVertexProgramCounters();

// Completed 82248C80 descriptor plus an owned draw-time copy of the original
// CPU constant bank (device+1920). These are bindings, not a decoded GPU shader.
struct EngineVertexBindingState {
    EngineVertexProgramKey key{};
    EngineVertexDescriptor descriptor;
    uint32_t deviceAddress = 0, recordAddress = 0, bindingAddress = 0, matrixAddress = 0;
    uint32_t descriptorAddress = 0; // Provenance only; the guest stack is reusable.
    bool operator==(const EngineVertexBindingState&) const = default;
};
struct EngineVertexBindingSnapshot : EngineVertexBindingState {
    std::array<uint8_t, 4096> constantBytes{}; // 256 big-endian float4 vectors.
    bool operator==(const EngineVertexBindingSnapshot&) const = default;
};
// Uses this thread's last successfully completed original preparation. Checks
// the current source/key/device/selected binding and rereads constant bytes.
// Failure preserves output; reused stack memory is never consulted at draw time.
bool snapshotEngineVertexBindings(uint8_t* base, EngineVertexBindingSnapshot& output) noexcept;
// Capture directly into an unpublished draw. Failure clears the optional;
// callers must not use it until this function has succeeded.
bool captureEngineVertexBindings(uint8_t* base, std::optional<EngineVertexBindingSnapshot>& output) noexcept;
}
