#pragma once
// Screen-space depth fog: shared math and engine-side run state.
//
// The original fog pass redraws every depth-prepass object with the fragment
// program XREngine_DepthFog (depth EQUAL, stencil EQUAL then INCR, blended).
// Both of its interpolants are "linear" texgen stages of the template vertex
// program: four DP4s of the post-skin position R8 with c[p..p+3] and
// c[q..q+3], while the clip position is c0..c3 * (R8 + c7). For every object
// of one view the stage is therefore the same linear function N of the clip
// position. One draw's constants give N, and N gives the fog inputs of any
// pixel from its position and depth. The native renderer then needs a single
// pass over the eligible pixels instead of one draw per object.
#include "world_mesh.h"
#include "engine_vertex_program.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace DarkRecomp::Native {
struct ScreenFogMapping {
    double inverse[4][4]{};    // clip position -> (R8,1), homogeneous
    double stages[2][4][4]{};  // texgen stage rows as a function of the clip position
};
inline bool screenFogInvert(const double a[4][4], double out[4][4]) noexcept {
    double m[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) m[i][j] = j < 4 ? a[i][j] : (j - 4 == i ? 1.0 : 0.0);
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row) if (std::abs(m[row][column]) > std::abs(m[pivot][column])) pivot = row;
        if (!(std::abs(m[pivot][column]) > 0) || !std::isfinite(m[pivot][column])) return false;
        if (pivot != column) for (int j = 0; j < 8; ++j) { const double t = m[pivot][j]; m[pivot][j] = m[column][j]; m[column][j] = t; }
        const double scale = 1.0 / m[column][column];
        for (int j = 0; j < 8; ++j) m[column][j] *= scale;
        for (int row = 0; row < 4; ++row) if (row != column) {
            const double factor = m[row][column];
            if (factor != 0) for (int j = 0; j < 8; ++j) m[row][j] -= factor * m[column][j];
        }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        out[i][j] = m[i][j + 4];
        if (!std::isfinite(out[i][j])) return false;
    }
    return true;
}
// Both stages "linear" (template mode 1) without a texture matrix; their rows
// start at the stage's TEXPARAM register.
inline bool screenFogStageRegisters(const WorldVertexOptions& options, const WorldVertexConstants& constants,
                                    uint32_t& first, uint32_t& second) noexcept {
    if (options.modes[0] != 1 || options.modes[1] != 1 || options.matrices[0] || options.matrices[1]) return false;
    first = constants.references[1][2]; second = constants.references[2][2];
    return first <= 252 && second <= 252;
}
// clip = m * (R8,1): rows c0..c3 with the template's "R4 = R8 + c7" folded in.
inline void screenFogClipMatrix(const WorldVertexConstants& constants, double m[4][4]) noexcept {
    const auto& c = constants.vectors;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) m[i][j] = c[i][j];
        m[i][3] = double(c[i][3]) * (1.0 + double(c[7][3])) + double(c[i][0]) * c[7][0] + double(c[i][1]) * c[7][1] + double(c[i][2]) * c[7][2];
    }
}
inline bool screenFogMappingFrom(const WorldVertexOptions& options, const WorldVertexConstants& constants,
                                 ScreenFogMapping& out) noexcept {
    uint32_t registers[2];
    if (!screenFogStageRegisters(options, constants, registers[0], registers[1])) return false;
    double clip[4][4];
    screenFogClipMatrix(constants, clip);
    if (!screenFogInvert(clip, out.inverse)) return false;
    for (int stage = 0; stage < 2; ++stage) for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        double sum = 0;
        for (int k = 0; k < 4; ++k) sum += double(constants.vectors[registers[stage] + i][k]) * out.inverse[k][j];
        if (!std::isfinite(sum)) return false;
        out.stages[stage][i][j] = sum;
    }
    return true;
}
// A draw belongs to the reference's view when its own stage rows equal
// reference.stages * its clip matrix. The bound is relative to each row's
// largest coefficient, with a floor relative to the whole stage for rows that
// are exactly zero.
inline bool screenFogConsistent(const WorldVertexOptions& options, const WorldVertexConstants& constants,
                                const ScreenFogMapping& reference, double tolerance) noexcept {
    uint32_t registers[2];
    if (!screenFogStageRegisters(options, constants, registers[0], registers[1])) return false;
    double clip[4][4];
    screenFogClipMatrix(constants, clip);
    for (int stage = 0; stage < 2; ++stage) {
        double stageScale = 0;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
            stageScale = (std::max)(stageScale, std::abs(double(constants.vectors[registers[stage] + i][j])));
        for (int i = 0; i < 4; ++i) {
            double rowScale = 0, worst = 0;
            for (int j = 0; j < 4; ++j) {
                double expected = 0;
                for (int k = 0; k < 4; ++k) expected += reference.stages[stage][i][k] * clip[k][j];
                const double actual = constants.vectors[registers[stage] + i][j];
                rowScale = (std::max)(rowScale, std::abs(actual));
                worst = (std::max)(worst, std::abs(expected - actual));
            }
            if (!(worst <= tolerance * rowScale + 1e-6 * stageScale)) return false;
        }
    }
    return true;
}
// Rows for the pixel shader. With v = (x, y, depth, 1) in target pixels and
// stored depth, stage s lane i is dot(rows[s*4+i], v) / dot(rows[8], v).
// viewport is {x, y, width, height} in target pixels; the stored depth is
// depthRange[0] + (depthRange[1] - depthRange[0]) * z/w as in rasterMain.
inline bool screenFogPixelRows(const ScreenFogMapping& mapping, const std::array<double,4>& viewport,
                               double depthNear, double depthFar, float rows[9][4]) noexcept {
    if (!(viewport[2] > 0) || !(viewport[3] > 0) || !(std::abs(depthFar - depthNear) > 0)) return false;
    const double toClip[4][4]{
        {2.0 / viewport[2], 0, 0, -2.0 * viewport[0] / viewport[2] - 1.0},
        {0, -2.0 / viewport[3], 0, 2.0 * viewport[1] / viewport[3] + 1.0},
        {0, 0, 1.0 / (depthFar - depthNear), -depthNear / (depthFar - depthNear)},
        {0, 0, 0, 1}};
    double result[9][4];
    for (int row = 0; row < 9; ++row) for (int j = 0; j < 4; ++j) {
        const double* left = row < 8 ? mapping.stages[row / 4][row % 4] : mapping.inverse[3];
        double sum = 0;
        for (int k = 0; k < 4; ++k) sum += left[k] * toClip[k][j];
        result[row][j] = sum;
    }
    // The quotient is scale-free; keep the denominator near one for float32.
    double scale = 0;
    for (int j = 0; j < 4; ++j) scale = (std::max)(scale, std::abs(result[8][j]));
    if (!(scale > 0) || !std::isfinite(scale)) return false;
    for (int row = 0; row < 9; ++row) for (int j = 0; j < 4; ++j) {
        const double value = result[row][j] / scale;
        if (!std::isfinite(value) || std::abs(value) > 3.0e38) return false;
        rows[row][j] = float(value);
    }
    return true;
}

// A draw whose clip z row equals its w row has z/w = 1 for every vertex: all
// of its fragments store one depth, which need not be its true distance (the
// engine's backdrop behind the world). Its fog inputs cannot be recovered
// from the stored depth, so it always stays a per-object draw.
inline bool screenFogConstantDepth(const WorldVertexConstants& constants) noexcept {
    return std::memcmp(&constants.vectors[2], &constants.vectors[3], sizeof(constants.vectors[2])) == 0;
}
// Identity of a stored draw's geometry: resource ids and index range, as the
// engine names them before capture and as the capture keeps them.
inline uint64_t screenFogGeometryKey(uint32_t vertices, uint32_t indices, uint32_t first, uint32_t count) noexcept {
    uint64_t x = (uint64_t(vertices) << 32 | indices) * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t(first) << 32 | count) * 0xC2B2AE3D27D4EB4Full;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return x | 1;
}
// User clip planes act alike on two draws: same enabled planes, same
// equations for those planes.
inline bool screenFogSameClip(const WorldClipPlanes& a, const WorldClipPlanes& b) noexcept {
    const uint32_t mask = a.enabledMask();
    if (mask != b.enabledMask()) return false;
    for (unsigned plane = 0; plane < a.planes.size(); ++plane)
        if ((mask & (1u << plane)) && std::memcmp(&a.planes[plane], &b.planes[plane], sizeof(a.planes[plane]))) return false;
    return true;
}
// Device state a fog draw's capture reads, as guest bytes. Bytes 76..79 of
// the attribute block hold a per-object pointer and are zeroed.
struct ScreenFogRawState {
    std::array<uint8_t,160> attributes{};
    std::array<uint8_t,24> viewport{};
    std::array<uint8_t,20> objects{};
    std::array<uint8_t,24> state{};
    std::array<uint8_t,64> fragment{};
    std::array<uint8_t,48> samplers{};
    std::array<uint8_t,8> bound{};
    uint32_t clipControl = 0, fragmentBytes = 0, textureMask = 0;
};
struct ScreenFogReference {
    uint32_t device = 0;
    ScreenFogRawState raw{};
    ScreenFogMapping mapping{};
};
// Relative bound for screenFogConsistent. Village scenes measure up to 2e-4
// for objects of the same view (float32 matrix products in the guest).
inline constexpr double screenFogTolerance = 2e-3;
// Engine thread. Zero when the draw can stand for its whole run; otherwise the
// first failed requirement (diagnostics only).
int screenFogReferenceFrom(const uint8_t* base, const EngineVertexBindingState& binding, const WorldDraw& draw,
                           ScreenFogReference& out) noexcept;
// Engine thread, before any capture: the device state a capture would read
// is byte for byte the reference's.
bool screenFogRawMatches(const uint8_t* base, const ScreenFogReference& reference) noexcept;
// ...and the bound vertex program's constants reproduce its mapping.
bool screenFogMatches(const uint8_t* base, const EngineVertexBindingState& binding, const ScreenFogReference& reference) noexcept;
// One element of a queued record's stored-geometry chain, as 82299EA0 passes
// it to the indexed subset draw 8225E2F0(vertices, indices, triangles,
// first). The original steps over an element without a vertex resource.
struct ScreenFogQueuedElement { uint16_t vertices = 0, indices = 0, triangles = 0, first = 0; };
// The elements, in order, of a record that 82299EA0 executes through
// 8225E2F0 alone: flag 8 selects the element chain at record+4, and every
// element has kind -4 or no vertex resource. False for any other record, an
// unreadable one, or a chain longer than limit.
bool screenFogQueuedElements(const uint8_t* base, uint32_t record, ScreenFogQueuedElement* out, size_t limit,
                             size_t& count) noexcept;
// A captured same-pass fog draw whose device bytes differ from the reference
// but whose decoded state and mapping agree: adopt its bytes for later draws.
bool screenFogRebase(const uint8_t* base, const EngineVertexBindingState& binding, const WorldDraw& held,
                     const WorldDraw& draw, ScreenFogReference& reference) noexcept;
}
