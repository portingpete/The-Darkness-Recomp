#include "renderer/engine/engine_texture_constants.h"
#include "renderer/engine/render_trace.h"
#include "renderer/engine/simple_mesh.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_8224A2E8);
constexpr uint32_t fixture = 0x03000000, extent = 65536, context = 0x82A69B00;
constexpr uint32_t descriptor = fixture + 0x100, cursor = fixture + 0x200, pointers = fixture + 0x300;
constexpr uint32_t attributes = fixture + 0x400, matrices = fixture + 0x1000;
constexpr uint32_t parameters = fixture + 0x2000, device = fixture + 0x4000;
static uint64_t cases = 0, comparisons = 0;
static void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
static void put32(uint8_t* b, uint32_t a, uint32_t value) {
    for (unsigned c = 0; c < 4; ++c) b[a + c] = uint8_t(value >> ((3 - c) * 8));
}
static PPCContext setup(uint8_t* base, const PPCContext& initial, const EngineTextureInput& input) {
    std::memset(base + fixture, 0xA5, extent);
    put32(base, context + 15748, device);
    base[descriptor + 20] = 0; base[descriptor + 21] = input.enabled ? 64 : 0;
    put32(base, descriptor, 0x76543210); put32(base, descriptor + 16, 0x00AABBCC);
    put32(base, cursor, fixture + 0xF000);
    put32(base, attributes + 44, input.componentMask);
    put32(base, attributes + 76, input.hasParameters ? parameters : 0);
    for (unsigned s = 0; s < 8; ++s) {
        base[attributes + 48 + s] = input.modes[s];
        put32(base, pointers + s * 4, input.matrices[s] ? matrices + s * 64 : 0);
        if (input.matrices[s]) for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
            put32(base, matrices + s * 64 + r * 16 + c * 4, std::bit_cast<uint32_t>((*input.matrices[s])[r][c]));
    }
    for (unsigned v = 0; v < input.parameterCount; ++v) for (unsigned c = 0; c < 4; ++c)
        put32(base, parameters + v * 16 + c * 4, input.parameters[v][c]);
    PPCContext guest; std::memcpy(&guest, &initial, sizeof(guest));
    guest.r3.u32 = descriptor; guest.r4.u32 = cursor; guest.r5.u32 = 12;
    guest.r6.u32 = pointers; guest.r7.u32 = attributes; guest.lr = 0x822490D4;
    return guest;
}
static void runOracle(uint8_t* base, const PPCContext& initial, const EngineTextureInput& input) {
    auto guest = setup(base, initial, input);
    const std::vector<uint8_t> before(base + fixture, base + fixture + extent);
    const unsigned callerMode = _mm_getcsr();
    EngineTextureObservation observation;
    require(beginEngineTextureObservation(base, descriptor, cursor, pointers, attributes, observation), "Valid observation rejected");
    require(_mm_getcsr() == callerMode && !std::memcmp(before.data(), base + fixture, extent), "Begin observation changed host FP or guest data");
    EngineTextureConstants host;
    require(buildEngineTextureConstants(input, host), "Valid owned input rejected");
    require(_mm_getcsr() == callerMode, "Builder changed host FP state");
    __imp__sub_8224A2E8(guest, base);
    const std::vector<uint8_t> original(base + fixture, base + fixture + extent);
    const unsigned afterMode = _mm_getcsr();
    finishEngineTextureObservation(base, guest.r3.u32, observation);
    require(_mm_getcsr() == afterMode && !std::memcmp(original.data(), base + fixture, extent), "Finish observation changed host FP or guest data");
    if (observation.comparison != TransformComparison::equal) {
        std::fprintf(stderr, "case=%llu count original=%u host=%u modes=", cases, guest.r3.u32, host.vectorCount);
        for (auto mode : input.modes) std::fprintf(stderr, "%u,", mode);
        std::fprintf(stderr, " mask=%08X source=%u\n", input.componentMask, input.hasParameters);
        require(false, "Owned texture constants/descriptor/cursor differ from original 8224A2E8");
    }
    require(guest.r3.u32 == host.vectorCount, "Original vector count differs");
    unsigned vector = 0;
    for (unsigned s = 0; s < 8; ++s) {
        const auto& stage = host.stages[s];
        auto check = [&](const EngineConstantWords& words) {
            for (unsigned c = 0; c < 4; ++c) {
                require(memory->read32(device + 2112 + vector * 16 + c * 4) == words[c], "Original constant words differ");
                ++comparisons;
            }
            ++vector;
        };
        if (stage.matrixColumns) {
            require(base[descriptor + 60 + s] == stage.matrixReference, "Matrix descriptor reference differs");
            for (const auto& column : *stage.matrixColumns) {
                EngineConstantWords words;
                for (unsigned c = 0; c < 4; ++c) words[c] = std::bit_cast<uint32_t>(column[c]);
                check(words);
            }
        }
        for (unsigned v = 0; v < stage.parameterCount; ++v) check(stage.parameters[v]);
    }
    // Retention and mismatch classification must not depend on live sources.
    if (cases % 101 == 0 && host.vectorCount) {
        const auto retained = observation.constants.stages[0].parameters;
        put32(base, parameters, 0); put32(base, device + 2112, memory->read32(device + 2112) ^ 1);
        require(observation.constants.stages[0].parameters == retained, "Observation retained guest parameter storage");
        // Restore sources so only output differs.
        std::memcpy(base + parameters, original.data() + parameters - fixture, 1280);
        finishEngineTextureObservation(base, guest.r3.u32, observation);
        require(observation.comparison == TransformComparison::different, "Mismatched output reported equal");
    }
    ++cases;
}
static void abi(uint8_t* base, const PPCContext& initial, const EngineTextureInput& input, uint32_t caller) {
    auto guest = setup(base, initial, input); guest.lr = caller;
    const uint32_t stack = guest.r1.u32 - 4096;
    const std::vector<uint8_t> before(base + fixture, base + fixture + extent), stackBefore(base + stack, base + stack + 4224);
    PPCContext expected; std::memcpy(&expected, &guest, sizeof(guest));
    __imp__sub_8224A2E8(expected, base);
    const unsigned expectedMode = _mm_getcsr();
    const std::vector<uint8_t> after(base + fixture, base + fixture + extent), stackAfter(base + stack, base + stack + 4224);
    std::memcpy(base + fixture, before.data(), extent); std::memcpy(base + stack, stackBefore.data(), stackBefore.size());
    PPCContext actual; std::memcpy(&actual, &guest, sizeof(guest));
    sub_8224A2E8(actual, base);
    require(!std::memcmp(&expected, &actual, sizeof(actual)), "Wrapper changed original PPC register ABI");
    require(!std::memcmp(after.data(), base + fixture, extent), "Wrapper changed original guest output");
    require(!std::memcmp(stackAfter.data(), base + stack, stackAfter.size()), "Wrapper changed original guest stack");
    require(_mm_getcsr() == expectedMode, "Wrapper changed original host FP state");
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected game directory");
        Memory owner; memory = &owner; owner.load(argv[1]); PPCContext initial{}; owner.initThread(initial);
        require(owner.commit(fixture, extent), "Cannot commit fixture");
        auto* base = owner.base(); configureRenderTrace({}); enableEnginePreview();
        require(owner.read32(0x8209DCBC) == 0 && owner.read32(0x8205C06C) == 0x3F800000 &&
                owner.read32(0x8209DE94) == 0x3B800000, "Original default constants differ");
        require(owner.read32(0x822490D0) == 0x48001219, "Original texture preparation call site differs (LR must be 822490D4)");
        EngineTextureInput input; input.enabled = true; input.parameterCount = 80;
        uint32_t random = 0x98212346;
        auto next = [&] { random = random * 1664525 + 1013904223; return random; };
        for (auto& vector : input.parameters) for (auto& value : vector) value = next();
        EngineMatrix matrix{};
        for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) matrix[r][c] = float(int32_t(next()) % 32768) / 16;
        matrix[0][0] = -0.0f; matrix[2][1] = std::bit_cast<float>(1u);
        for (unsigned mode = 0; mode <= 22; ++mode) for (unsigned s = 0; s < 8; ++s)
            for (bool source : {false, true}) for (bool haveMatrix : {false, true})
                for (unsigned mask = 0; mask < (mode == 1 ? 16u : 1u); ++mask) {
                    input.modes.fill(4); input.modes[s] = uint8_t(mode); input.matrices.fill(std::nullopt);
                    if (haveMatrix) input.matrices[s] = matrix;
                    input.componentMask = mask << (s * 4); input.hasParameters = source;
                    runOracle(base, initial, input);
                }
        for (unsigned sample = 0; sample < 512; ++sample) {
            input.hasParameters = sample % 2; input.componentMask = next();
            for (unsigned s = 0; s < 8; ++s) {
                input.modes[s] = uint8_t(next() % 23);
                input.matrices[s] = next() & 4 ? std::optional(matrix) : std::nullopt;
            }
            runOracle(base, initial, input);
        }
        // Maximum original output (112 vectors), plus the early-out contract.
        input.hasParameters = true; input.modes.fill(3); input.matrices.fill(matrix); runOracle(base, initial, input);
        input.enabled = false; runOracle(base, initial, input);
        auto guest = setup(base, initial, input);
        EngineTextureObservation disabled;
        require(beginEngineTextureObservation(base, descriptor, cursor, 0xFFFFFFFF, 0xFFFFFFFF, disabled), "Early-out read unused pointers");
        __imp__sub_8224A2E8(guest, base); finishEngineTextureObservation(base, guest.r3.u32, disabled);
        require(disabled.comparison == TransformComparison::equal, "Early-out changed descriptor/cursor");
        input.enabled = true;
        for (unsigned mode = 0; mode <= 22; ++mode) for (bool source : {false, true}) {
            input.modes.fill(uint8_t(mode)); input.hasParameters = source; input.componentMask = 0x59A5FC3E;
            abi(base, initial, input, 0x822490D4); abi(base, initial, input, 0x12345678);
        }
        for (unsigned kind = 0; kind < 3; ++kind) {
            EngineTextureInput invalid = input;
            if (kind == 0) invalid.modes[7] = 23;
            if (kind == 1) { invalid.modes.fill(3); invalid.hasParameters = true; invalid.parameterCount = 79; }
            if (kind == 2) (*invalid.matrices[3])[0][1] = std::bit_cast<float>(0x7F800001u);
            EngineTextureConstants output; output.vectorCount = 999;
            const unsigned mode = _mm_getcsr();
            require(!buildEngineTextureConstants(invalid, output) && output.vectorCount == 999, "Invalid input accepted or changed output");
            require(_mm_getcsr() == mode, "Invalid input changed host FP state");
        }
        input.modes.fill(3); input.hasParameters = true;
        for (unsigned kind = 0; kind < 6; ++kind) {
            setup(base, initial, input); EngineTextureObservation output; output.initialCursor = 999;
            if (kind == 0) put32(base, pointers, 0xFFFFFFF0);
            if (kind == 1) put32(base, attributes + 76, parameters + 1);
            if (kind == 2) put32(base, attributes + 76, 0xFFFFFFF0);
            if (kind == 3) put32(base, context + 15748, 0xFFFFFFF0);
            require(!beginEngineTextureObservation(base, kind == 4 ? 0xFFFFFFF0 : descriptor, cursor, pointers,
                kind == 5 ? 0xFFFFFFF0 : attributes, output) && output.initialCursor == 999, "Invalid bounds accepted or changed observation");
        }
        guest = setup(base, initial, input); EngineTextureObservation changed;
        require(beginEngineTextureObservation(base, descriptor, cursor, pointers, attributes, changed), "Valid snapshot rejected");
        __imp__sub_8224A2E8(guest, base); base[attributes + 48] = 4;
        finishEngineTextureObservation(base, guest.r3.u32, changed);
        require(changed.comparison == TransformComparison::unavailable, "Changed source compared as original");
        std::printf("EngineTextureContract passed: %llu original-AOT cases, %llu bit-exact constant words; all modes 0..22 at all eight stages, all mode-1 masks, matrices/defaults/owned parameters, mixed stages, 112-vector maximum, early-out, 92 wrapper ABI cases, bounds, FP state and mismatch classification.\n", cases, comparisons);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "EngineTextureContract failed: %s\n", e.what()); return 1; }
}
