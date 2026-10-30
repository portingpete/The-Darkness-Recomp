#pragma once
// Native graphics layer v1 (private). Lean stored-draw capture: one SEH guard per
// stage, direct big-endian guest loads, no 4 KiB constant-bank copy, no redundant
// re-reads of engine-thread-owned device state. Every unusual case (fault, reject,
// diagnostic log, palette fallback, streaming race) returns to the original path.
#include "world_mesh.h"
#include "stored_geometry.h"
#include "engine_vertex_program.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace DarkRecomp::Native {
// DARK_NATIVE_LAYER: 0 = original capture, 1 = native layer (default), 2 = check
// (run both, compare every field, queue the original result).
enum class NativeLayerMode : unsigned { off = 0, on = 1, check = 2 };
NativeLayerMode nativeLayerMode() noexcept;

// Guest memory helpers. Valid when the original guarded copy would be: nonzero
// address and address + size within the 32-bit guest space.
inline bool nativeGuestRange(uint64_t address, uint64_t size) noexcept {
    return address && address < 0x100000000ull && size <= 0x100000000ull - address;
}
inline uint32_t nativeGuest32(const uint8_t* base, uint32_t address) noexcept {
    uint32_t value; std::memcpy(&value, base + address, 4); return _byteswap_ulong(value);
}
inline uint16_t nativeGuest16(const uint8_t* base, uint32_t address) noexcept {
    uint16_t value; std::memcpy(&value, base + address, 2); return _byteswap_ushort(value);
}

enum class NativeLayerResult : uint8_t { ok, empty, fallback };
// Stored geometry with direct guarded validation under one cache lock.
NativeLayerResult storedGeometryDrawFast(StoredGeometryCache&, uint8_t* base, uint32_t vertexId, uint32_t indexId,
                                         uint32_t firstIndex, uint32_t indexCount, uint32_t boundIndexBuffer,
                                         StoredDraw& output) noexcept;
NativeLayerResult resolveStoredDrawFast(const StoredDrawRequest&, StoredGeometryCache&, uint8_t* base, uint32_t returnAddress,
                                        uint32_t primitive, uint32_t baseVertex, uint32_t first, uint32_t count,
                                        uint32_t boundVertexId, uint32_t boundIndexBuffer, StoredDraw& output) noexcept;
// Current completed vertex-program binding, checked with direct loads and without
// copying the bank. ok fills output; empty means the original capture would fail.
NativeLayerResult currentEngineVertexBindingFast(uint8_t* base, EngineVertexBindingState& output) noexcept;
// Fast snapshot. Constants are read from the live device bank (device+1920) through
// the original preparation recipe. On fallback the geometry is left untouched.
std::shared_ptr<WorldDraw> captureWorldDrawFast(uint8_t* base, const EngineVertexBindingState& binding,
                                                StoredDraw& geometry) noexcept;
// Field-by-field (bitwise for floats) comparison for the check mode.
bool sameCapturedWorldDraw(const WorldDraw& a, const WorldDraw& b, const char*& field) noexcept;
// Queue an already captured draw through the unchanged budget/queue code.
void previewQueueCapturedWorld(uint8_t* base, std::shared_ptr<WorldDraw> draw);
// The complete stored-draw observation: native layer with exact fallback.
void nativeLayerObserveStoredDraw(uint8_t* base, const StoredDrawRequest& request, uint32_t returnAddress,
                                  uint32_t primitive, uint32_t baseVertex, uint32_t first, uint32_t count,
                                  uint32_t boundVertexId, uint32_t boundIndexBuffer) noexcept;
void printNativeLayerCounters();
// v1b: retained vertex binding after 82248C80 with direct loads (exact fallback), the immediate
// world path through the fast binding/snapshot, and check-mode reporting for both.
void retainCompletedVertexBindingLayer(uint8_t* base, uint32_t stack) noexcept;
bool nativeLayerObserveImmediate(uint8_t* base, uint32_t device, StoredDraw& geometry) noexcept;
void nativeLayerReportCheck(const char* kind, bool same) noexcept;
// v2: dry-run Xbox packet payload emitters for the draw paths (render_trace.cpp).
void printNativeEmitterCounters();
}
