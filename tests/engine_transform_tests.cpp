#include "renderer/engine/engine_transforms.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_82248A78);
constexpr uint32_t fixture = 0x03000000, extent = 65536, context = 0x82A69B00;
constexpr uint32_t matrixBase = fixture + 0x1000, selected = matrixBase + 656 + 16;
constexpr uint32_t device = fixture + 0x4000;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void put32(uint8_t* base, uint32_t address, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) base[address + i] = uint8_t(v >> ((3 - i) * 8));
}
static void writeMatrix(uint8_t* base, uint32_t address, const EngineMatrix& matrix) {
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) put32(base, address + r * 16 + c * 4, std::bit_cast<uint32_t>(matrix[r][c]));
}
static EngineMatrix identity() {
    EngineMatrix m{}; for (unsigned i = 0; i < 4; ++i) m[i][i] = 1; return m;
}
static void setInput(uint8_t* base, const EngineTransformInput& input) {
    put32(base, context + 8224, matrixBase); put32(base, context + 8232, 1);
    put32(base, context + 15748, device);
    writeMatrix(base, selected, input.modelView); writeMatrix(base, context + 17088, input.projection);
}
static void oracle(uint8_t* base, const PPCContext& initial) {
    PPCContext guest; std::memcpy(&guest, &initial, sizeof(guest));
    guest.fpscr.loadFromHost();
    __imp__sub_82248A78(guest, base);
}
static void contracts(uint8_t* base, const PPCContext& initial) {
    require(memory->read32(0x82A47230) == 0 && memory->read32(0x82A47234) == 0 &&
            memory->read32(0x82A47238) == 0 && memory->read32(0x82A4723C) == 0x3F800000 &&
            memory->read32(0x82A47280) == 0xFFFFFFFF && memory->read32(0x82A47284) == 0xFFFFFFFF &&
            memory->read32(0x82A47288) == 0xFFFFFFFF && memory->read32(0x82A4728C) == 0,
            "Original transform constant vectors differ");
    uint32_t random = 0x42916A7B;
    auto next = [&] { random = random * 1664525 + 1013904223; return float(int32_t(random) % 1000000) / 8192.0f; };
    uint64_t comparisons = 0;
    for (unsigned sample = 0; sample < 1024; ++sample) {
        EngineTransformInput input{identity(), identity()};
        if (sample == 1) input.modelView[3] = {37.25f, -12.5f, 0.125f, 1};
        else if (sample == 2) {
            input.modelView = {{{0, 2, 0, 0}, {-3, 0, 0, 0}, {0, 0, 4, 0}, {7, -8, 9, 1}}};
            input.projection = {{{1.5f, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 1.01f, 1}, {0, 0, -0.101f, 0}}};
        } else if (sample >= 3) {
            for (auto& row : input.modelView) for (float& v : row) v = next();
            for (auto& row : input.projection) for (float& v : row) v = next();
            if (sample % 4 == 0) { for (unsigned i = 0; i < 3; ++i) input.modelView[i][3] = 0; input.modelView[3][3] = 1; }
            if (sample % 7 == 0) input.modelView[0][1] = -0.0f;
            if (sample % 11 == 0) input.projection[2][2] = std::numeric_limits<float>::denorm_min();
        }
        setInput(base, input);
        oracle(base, initial);
        const unsigned modeBefore = _mm_getcsr();
        // Native arithmetic must not inherit a UI thread's rounding mode or
        // leak its flush/rounding state back to any caller.
        _mm_setcsr((modeBefore & ~0xE040u) | 0x2000u);
        const unsigned callerMode = _mm_getcsr();
        EngineTransformConstants host;
        require(buildEngineTransformConstants(input, host), "Valid transform rejected");
        require(_mm_getcsr() == callerMode, "Native transform changed host FP state");
        _mm_setcsr(modeBefore);
        for (unsigned row = 0; row < 8; ++row) for (unsigned c = 0; c < 4; ++c) {
            const uint32_t expected = memory->read32(device + 1920 + row * 16 + c * 4);
            const uint32_t actual = std::bit_cast<uint32_t>(host.vectors[row][c]);
            if (expected != actual) {
                std::fprintf(stderr, "sample=%u vector=%u lane=%u original=%08X host=%08X\n", sample, row, c, expected, actual);
                require(false, "Host transform differs from original 82248A78");
            }
            ++comparisons;
        }
        if (sample < 3) {
            const std::vector<uint8_t> before(base + fixture, base + fixture + extent);
            const std::vector<uint8_t> contextBefore(base + context, base + context + 18000);
            EngineTransformSnapshot snapshot;
            require(snapshotEngineTransforms(base, snapshot) && snapshot.originalComparison == TransformComparison::equal &&
                    snapshot.matrixAddress == selected && snapshot.deviceAddress == device,
                    "Original engine transform snapshot did not match");
            require(!std::memcmp(before.data(), base + fixture, extent) &&
                    !std::memcmp(contextBefore.data(), base + context, contextBefore.size()), "Snapshot modified guest memory");
            const auto retained = snapshot.constants;
            put32(base, selected, 0); put32(base, device + 1920, 0);
            require(!std::memcmp(&retained, &snapshot.constants, sizeof(retained)), "Snapshot retained live guest memory");
        }
    }
    EngineTransformInput valid{identity(), identity()};
    EngineTransformConstants sentinel;
    for (auto& row : sentinel.vectors) row = {91, 92, 93, 94};
    for (unsigned kind = 0; kind < 5; ++kind) {
        auto invalid = valid;
        if (kind == 0) invalid.modelView[0][0] = std::numeric_limits<float>::infinity();
        if (kind == 1) invalid.projection[1][2] = std::numeric_limits<float>::quiet_NaN();
        if (kind == 2) for (auto& row : invalid.modelView) row[0] = 0;
        if (kind == 3) invalid.modelView[0][0] = std::numeric_limits<float>::max();
        if (kind == 4) invalid.modelView[0][0] = std::numeric_limits<float>::min();
        auto output = sentinel;
        require(!buildEngineTransformConstants(invalid, output) && !std::memcmp(&output, &sentinel, sizeof(output)),
                "Invalid transform accepted or changed output");
    }
    setInput(base, valid); oracle(base, initial);
    EngineTransformSnapshot output;
    require(snapshotEngineTransforms(base, output), "Valid snapshot rejected");
    const auto untouched = output;
    for (unsigned kind = 0; kind < 4; ++kind) {
        setInput(base, valid);
        if (kind == 0) put32(base, context + 8224, 0);
        if (kind == 1) put32(base, context + 8232, 256);
        if (kind == 2) put32(base, context + 8224, 0xFFFFFFF0);
        if (kind == 3) put32(base, context + 8224, matrixBase + 1);
        require(!snapshotEngineTransforms(base, output) && !std::memcmp(&output, &untouched, sizeof(output)),
                "Invalid source selection accepted or changed output");
    }
    setInput(base, valid); oracle(base, initial); put32(base, device + 1920, 0);
    require(snapshotEngineTransforms(base, output) && output.originalComparison == TransformComparison::different,
            "Different original constants were reported as equal");
    put32(base, context + 15748, 0xFFFFFFF0);
    require(snapshotEngineTransforms(base, output) && output.originalComparison == TransformComparison::unavailable,
            "Unavailable original constants were reported as compared");
    std::printf("Engine transforms: %llu bit-exact float comparisons against original 82248A78 over 1024 identity, translation, rotation, scale, perspective, random and denormal cases.\n", comparisons);
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected original game directory");
        Memory owner; memory = &owner; owner.load(argv[1]); PPCContext initial{}; owner.initThread(initial);
        require(owner.commit(fixture, extent), "Cannot commit transform fixture");
        std::memset(owner.base() + fixture, 0, extent);
        contracts(owner.base(), initial);
        std::puts("EngineTransformContract passed: original-AOT output, owned snapshots, unchanged guest memory/host FP state, invalid bounds/data and comparison classification.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "EngineTransformContract failed: %s\n", e.what()); return 1; }
}
