#include "renderer/engine/engine_vertex_descriptor.h"
#include "renderer/engine/render_trace.h"
#include "renderer/engine/simple_mesh.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_8223AFE8);
extern "C" PPC_FUNC(__imp__sub_8224DC70);
extern "C" PPC_FUNC(__imp__sub_8224DAE0);
extern "C" PPC_FUNC(__imp__sub_8224A0A8);
constexpr uint32_t fixture = 0x03000000, extent = 65536, context = 0x82A69B00;
constexpr uint32_t descriptor = fixture + 0x100, attributes = fixture + 0x200, pointers = fixture + 0x300;
constexpr uint32_t cursor = fixture + 0x400, source = fixture + 0x1000, device = fixture + 0x4000;
static uint64_t descriptorCases = 0, conversionCases = 0, wordsCompared = 0, abiCases = 0;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void put32(uint8_t* base, uint32_t address, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) base[address + i] = uint8_t(value >> ((3 - i) * 8));
}
static PPCContext clone(const PPCContext& input) { PPCContext result; std::memcpy(&result, &input, sizeof(result)); return result; }
static PPCContext setup(uint8_t* base, const PPCContext& initial, const EngineVertexDescriptor& state) {
    std::memset(base + fixture, 0xA5, extent);
    const auto bytes = encodeEngineVertexDescriptor(state); std::memcpy(base + descriptor, bytes.data(), bytes.size());
    put32(base, context + 15748, device); put32(base, cursor, fixture + 0xF000);
    auto guest = clone(initial); guest.r3.u32 = descriptor; return guest;
}
static void abi(PPCFunc* original, PPCFunc* wrapped, PPCContext initial, uint8_t* base) {
    const uint32_t stack = initial.r1.u32 - 4096;
    std::vector<uint8_t> before(base + fixture, base + fixture + extent), stackBefore(base + stack, base + stack + 4224);
    auto expected = clone(initial); original(expected, base);
    const auto expectedMode = _mm_getcsr();
    std::vector<uint8_t> after(base + fixture, base + fixture + extent), stackAfter(base + stack, base + stack + 4224);
    std::memcpy(base + fixture, before.data(), extent); std::memcpy(base + stack, stackBefore.data(), stackBefore.size());
    auto actual = clone(initial); wrapped(actual, base);
    require(!std::memcmp(&expected, &actual, sizeof(actual)), "Observer changed original PPC register ABI");
    require(!std::memcmp(base + fixture, after.data(), extent), "Observer changed original guest output");
    require(!std::memcmp(base + stack, stackAfter.data(), stackAfter.size()), "Observer changed original guest stack");
    require(_mm_getcsr() == expectedMode, "Observer changed original host FP state");
    ++abiCases;
}
static void descriptorOracle(uint8_t* base, const PPCContext& initial, const EngineVertexDescriptor& state,
                             const EngineVertexDescriptorInput& input, bool checkAbi = false) {
    auto guest = setup(base, initial, state);
    for (unsigned s = 0; s < 8; ++s) {
        base[attributes + 48 + s] = input.modes[s];
        // This routine tests only pointer presence, never its pointee.
        put32(base, pointers + s * 4, input.matrixMask & (1u << s) ? 0xFFFFFFF0 : 0);
    }
    put32(base, attributes + 92, input.materialFlags);
    guest.r4.u32 = attributes; guest.r5.u32 = pointers; guest.r6.u32 = input.enabledCoordinates; guest.lr = 0x82248E98;
    const std::vector<uint8_t> before(base + fixture, base + fixture + extent);
    const unsigned mode = _mm_getcsr();
    EngineVertexDescriptor expected;
    require(buildEngineVertexDescriptor(state, input, expected), "Valid descriptor input rejected");
    EngineDescriptorObservation observation;
    require(beginEngineDescriptorObservation(base, descriptor, attributes, pointers, input.enabledCoordinates, observation), "Valid descriptor observation rejected");
    require(_mm_getcsr() == mode && !std::memcmp(base + fixture, before.data(), extent), "Descriptor builder/observation changed source or host FP");
    if (checkAbi) {
        abi(__imp__sub_8224DAE0, sub_8224DAE0, guest, base);
        std::memcpy(base + fixture, before.data(), extent); guest.lr = 0x12345678;
        abi(__imp__sub_8224DAE0, sub_8224DAE0, guest, base);
        std::memcpy(base + fixture, before.data(), extent); guest.lr = 0x82248E98;
    }
    __imp__sub_8224DAE0(guest, base);
    const auto bytes = encodeEngineVertexDescriptor(expected);
    require(!std::memcmp(base + descriptor, bytes.data(), 80), "Native descriptor differs from original 8224DAE0");
    const std::vector<uint8_t> after(base + fixture, base + fixture + extent);
    finishEngineDescriptorObservation(base, observation);
    require(observation.comparison == TransformComparison::equal, "Descriptor observation did not match original");
    require(!std::memcmp(base + fixture, after.data(), extent) && _mm_getcsr() == mode, "Finish descriptor observation changed guest/FP state");
    if (descriptorCases % 97 == 0) {
        base[descriptor + 4] ^= 1; finishEngineDescriptorObservation(base, observation);
        require(observation.comparison == TransformComparison::different, "Wrong descriptor reported equal");
        base[attributes + 48] ^= 1; finishEngineDescriptorObservation(base, observation);
        require(observation.comparison == TransformComparison::unavailable, "Changed descriptor source was compared");
    }
    ++descriptorCases;
}
static void conversionOracle(uint8_t* base, const PPCContext& initial, const EngineVertexDescriptor& state,
                             const std::array<EngineConstantWords, 18>& words, bool checkAbi = false) {
    auto guest = setup(base, initial, state);
    for (unsigned v = 0; v < words.size(); ++v) for (unsigned c = 0; c < 4; ++c) put32(base, source + v * 16 + c * 4, words[v][c]);
    guest.r4.u32 = cursor; guest.r5.u32 = 0xBAD; guest.r6.u32 = source; guest.lr = 0x822490B8;
    const std::vector<uint8_t> before(base + fixture, base + fixture + extent);
    const unsigned mode = _mm_getcsr(); EngineConversionConstants expected;
    require(buildEngineConversionConstants(state, words, 18, expected), "Valid conversion constants rejected");
    EngineConversionObservation observation;
    require(beginEngineConversionObservation(base, descriptor, cursor, source, observation), "Valid conversion observation rejected");
    require(_mm_getcsr() == mode && !std::memcmp(base + fixture, before.data(), extent), "Conversion builder/observation changed source or host FP");
    if (checkAbi) {
        abi(__imp__sub_8224A0A8, sub_8224A0A8, guest, base);
        std::memcpy(base + fixture, before.data(), extent); guest.lr = 0x12345678;
        abi(__imp__sub_8224A0A8, sub_8224A0A8, guest, base);
        std::memcpy(base + fixture, before.data(), extent); guest.lr = 0x822490B8;
    }
    __imp__sub_8224A0A8(guest, base);
    const std::vector<uint8_t> after(base + fixture, base + fixture + extent);
    const auto bytes = encodeEngineVertexDescriptor(expected.descriptor);
    require(!std::memcmp(base + descriptor, bytes.data(), 80) && guest.r3.u32 == expected.vectorCount,
            "Native conversion descriptor/count differs from original 8224A0A8");
    unsigned vector = 0;
    auto check = [&](const std::optional<EngineConversionPair>& pair) {
        if (!pair) return;
        for (const auto& row : {pair->scale, pair->offset}) {
            for (unsigned c = 0; c < 4; ++c) {
                require(memory->read32(device + 3136 + vector * 16 + c * 4) == row[c], "Native conversion pair bits differ");
                ++wordsCompared;
            }
            ++vector;
        }
    };
    check(expected.position); for (const auto& pair : expected.coordinates) check(pair);
    finishEngineConversionObservation(base, guest.r3.u32, observation);
    require(observation.comparison == TransformComparison::equal, "Conversion observation did not match original");
    require(!std::memcmp(base + fixture, after.data(), extent) && _mm_getcsr() == mode, "Finish conversion observation changed guest/FP state");
    if (expected.vectorCount && conversionCases % 97 == 0) {
        const auto retained = observation.source;
        base[device + 3136] ^= 1; finishEngineConversionObservation(base, guest.r3.u32, observation);
        require(observation.comparison == TransformComparison::different, "Wrong conversion output reported equal");
        base[source] ^= 1; finishEngineConversionObservation(base, guest.r3.u32, observation);
        require(observation.comparison == TransformComparison::unavailable, "Changed conversion source was compared");
        require(observation.source == retained, "Owned conversion source changed with guest source");
    }
    ++conversionCases;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected game directory");
        Memory owner; memory = &owner; owner.load(argv[1]); PPCContext initial{}; owner.initThread(initial);
        require(owner.commit(fixture, extent), "Cannot commit fixture"); auto* base = owner.base();
        configureRenderTrace({}); enableEnginePreview();
        require(owner.read32(0x82248E94) == 0x48004C4D && owner.read32(0x822490B4) == 0x48000FF5,
                "Original descriptor/conversion call instructions differ");
        auto initialization = clone(initial); __imp__sub_8223AFE8(initialization, base);
        for (unsigned i = 0; i < engineVertexModes().size(); ++i) {
            require(owner.read32(0x82A5CD88 + i * 4) == engineVertexModes()[i].flags &&
                    base[0x82A5CDF4 + i] == engineVertexModes()[i].reservation, "Recovered mode table differs from original initializer");
        }
        auto guest = setup(base, initial, {}); std::memset(base + descriptor, 0xA5, 80);
        __imp__sub_8224DC70(guest, base); const auto reset = encodeEngineVertexDescriptor({});
        require(!std::memcmp(base + descriptor, reset.data(), 80), "Native descriptor defaults differ from original 8224DC70");
        uint32_t random = 0xFAC68800;
        auto next = [&] { random = random * 1664525 + 1013904223; return random; };
        for (unsigned sample = 0; sample < 256; ++sample) {
            std::array<uint8_t, 80> bytes; for (auto& value : bytes) value = uint8_t(next() >> 24);
            require(encodeEngineVertexDescriptor(decodeEngineVertexDescriptor(bytes)) == bytes, "Opaque descriptor fields did not round-trip");
        }
        EngineVertexDescriptor state; state.reserved22 = 0x7391; state.reserved76 = 0xBADFADE;
        state.parameters[5] = {17,18,19,20}; state.conversions.fill(0xAC);
        EngineVertexDescriptorInput input;
        for (unsigned mode = 0; mode < 27; ++mode) for (unsigned s = 0; s < 8; ++s)
            for (unsigned variant = 0; variant < 16; ++variant) {
                input.modes.fill(4); input.modes[s] = uint8_t(mode);
                input.enabledCoordinates = variant & 1 ? 255 : 0; input.matrixMask = variant & 2 ? 1u << s : 0;
                input.materialFlags = variant & 4 ? 0x8000 : 0; state.flags = variant & 8 ? 0x812300AC : 0;
                descriptorOracle(base, initial, state, input, s == 0 && variant == 15);
            }
        for (unsigned sample = 0; sample < 1024; ++sample) {
            std::array<uint8_t, 80> bytes; for (auto& value : bytes) value = uint8_t(next() >> 24);
            state = decodeEngineVertexDescriptor(bytes);
            for (auto& mode : input.modes) mode = uint8_t(next() % 27);
            input.enabledCoordinates = next(); input.matrixMask = sample & 255; input.materialFlags = next();
            descriptorOracle(base, initial, state, input);
        }
        // Mode19 reserves one vector in DAE0, but A2E8 later writes two.
        input = {}; input.modes.fill(4); input.modes[3] = 19;
        EngineVertexDescriptor reservation;
        require(buildEngineVertexDescriptor({}, input, reservation) && reservation.textureReservation == 1, "Original mode19 reservation was normalized incorrectly");
        std::array<EngineConstantWords, 18> words;
        for (auto& row : words) for (auto& value : row) value = next();
        words[0] = {0, 0x80000000, 0x7F800001, 0xFFFFFFFF}; // Copy semantics preserve even non-float words.
        for (unsigned mask = 0; mask < 512; ++mask) for (unsigned mapping = 0; mapping < 4; ++mapping) {
            state = {}; state.flags = (mask & 255) | (mask & 256 ? 0x04000000 : 0) | 0x80001000;
            state.coordinateMapping = mapping == 0 ? 0xFAC68800 : mapping == 1 ? 0 : mapping == 2 ? 0xFFFFFFFF : next();
            state.positionConversion = 43; state.conversions.fill(33); state.reserved76 = next();
            conversionOracle(base, initial, state, words, mapping == 0 && mask % 17 == 0);
        }
        EngineConversionConstants sentinel; sentinel.vectorCount = 999;
        for (unsigned count : {0u,17u,19u}) {
            state = {}; state.flags = 0x040000FF;
            require(!buildEngineConversionConstants(state, words, count, sentinel) && sentinel.vectorCount == 999, "Short/oversize conversion input accepted or changed output");
        }
        EngineVertexDescriptor invalidOut; invalidOut.flags = 999; input.modes[7] = 27;
        require(!buildEngineVertexDescriptor({}, input, invalidOut) && invalidOut.flags == 999, "Unknown descriptor mode accepted or changed output");
        state = {}; state.flags = 0x040000FF;
        for (unsigned kind = 0; kind < 5; ++kind) {
            setup(base, initial, state); EngineConversionObservation output; output.initialCursor = 999;
            if (kind == 4) put32(base, context + 15748, 0xFFFFFFF0);
            require(!beginEngineConversionObservation(base, kind == 0 ? 0xFFFFFFF0 : descriptor,
                kind == 1 ? 0xFFFFFFF0 : cursor, kind == 2 ? source + 1 : kind == 3 ? 0xFFFFFFF0 : source, output) &&
                output.initialCursor == 999, "Invalid conversion address accepted or changed output");
        }
        setup(base, initial, {}); EngineConversionObservation early;
        require(beginEngineConversionObservation(base, descriptor, cursor, 0xFFFFFFFF, early) && !early.constants.vectorCount,
                "Early-out read unused conversion source");
        EngineDescriptorObservation bad; bad.descriptorAddress = 999;
        require(!beginEngineDescriptorObservation(base, descriptor, 0xFFFFFFF0, pointers, 0, bad) && bad.descriptorAddress == 999,
                "Invalid attributes accepted or changed output");
        base[0x82A5CDF4 + 19] = 2;
        require(!beginEngineDescriptorObservation(base, descriptor, attributes, pointers, 0, bad), "Changed original mode table was silently accepted");
        std::printf("EngineVertexDescriptorContract passed: original 27-entry initialization/defaults; %llu descriptor cases (all modes/stages and mixed state), %llu conversion cases (all 512 masks), %llu bit-exact conversion words, %llu complete wrapper ABI cases; opaque fields, owned data, original mode19 reservation, bounds, FP state and comparison classification.\n",
                    descriptorCases, conversionCases, wordsCompared, abiCases);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "EngineVertexDescriptorContract failed: %s\n", e.what()); return 1; }
}
