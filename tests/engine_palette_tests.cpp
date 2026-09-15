#include "renderer/engine/engine_palette.h"
#include "renderer/engine/simple_mesh.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_8224AB18);
constexpr uint32_t fixture = 0x03000000, extent = 65536, context = 0x82A69B00;
constexpr uint32_t descriptor = fixture + 256, cursor = fixture + 512, source = fixture + 768;
constexpr uint32_t indices = fixture + 1024, matrices = fixture + 4096, device = fixture + 16384;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void put(uint8_t* b, uint32_t p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) b[p+i] = uint8_t(v >> (24-i*8)); }
static PPCContext clone(const PPCContext& c) { PPCContext out; std::memcpy(&out, &c, sizeof(out)); return out; }
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected game directory");
        Memory owner; memory = &owner; owner.load(argv[1]); PPCContext initial{}; owner.initThread(initial);
        require(owner.commit(fixture, extent), "Cannot commit fixture"); auto* base = owner.base();
        enableEnginePreview();
        require(owner.read32(0x8224910C) == 0x48001A0D, "Original palette call instruction differs");
        uint32_t random = 0x8224AB18;
        auto next = [&] { random = random * 1664525 + 1013904223; return random; };
        std::array<EngineMatrixWords, 80> originals;
        for (auto& matrix : originals) for (auto& row : matrix) for (auto& v : row) {
            const auto bits = next(); v = (bits & 0x807FFFFFu) | ((1 + next() % 254) << 23);
        }
        originals[0][0][0] = 0x80000000; originals[0][1][0] = 0;
        // Original never reads column3, including its nonfinite values.
        for (auto& matrix : originals) for (auto& row : matrix) row[3] = 0x7F800001;
        std::vector<uint32_t> counts; for (unsigned n = 0; n < 64; ++n) counts.push_back(n);
        for (auto n : {255u, 65536u, 0xFFFFFFFFu}) counts.push_back(n);
        uint64_t cases = 0, words = 0, abiCases = 0;
        for (const auto requested : counts) for (unsigned indexed = 0; indexed < 2; ++indexed) {
            std::memset(base + fixture, 0xA5, extent);
            put(base, context + 15748, device); put(base, source, matrices); put(base, source + 4, indexed ? indices : 0);
            put(base, source + 8, requested); put(base, cursor, 0xBADFADE);
            std::array<uint8_t, 80> opaque; for (auto& v : opaque) v = uint8_t(next() >> 24);
            const auto state = decodeEngineVertexDescriptor(opaque); std::memcpy(base + descriptor, opaque.data(), opaque.size());
            for (unsigned m = 0; m < originals.size(); ++m) for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
                put(base, matrices + m * 64 + r * 16 + c * 4, originals[m][r][c]);
            EnginePaletteInput input; input.requestedCount = requested;
            const auto count = std::min(requested, 52u);
            for (unsigned m = 0; m < count; ++m) {
                const unsigned index = indexed ? (m % 5 == 0 ? 0 : 79 - m) : m;
                base[indices + 2*m] = uint8_t(index >> 8); base[indices + 2*m + 1] = uint8_t(index);
                input.selected[m] = originals[index];
            }
            auto guest = clone(initial); guest.r3.u32 = descriptor; guest.r4.u32 = cursor;
            guest.r5.u32 = 0xBAD; guest.r6.u32 = 0xBAD; guest.r7.u32 = source; guest.lr = 0x82249110;
            if (!count) { guest.r4.u32 = 0xFFFFFFFF; put(base, source, 0xFFFFFFFF); put(base, source+4, 0xFFFFFFFF); }
            const auto mode = _mm_getcsr();
            const std::vector<uint8_t> before(base + fixture, base + fixture + extent);
            const auto stack = guest.r1.u32 - 4096;
            const std::vector<uint8_t> stackBefore(base + stack, base + stack + 4224);
            EnginePaletteConstants expected;
            require(buildEnginePaletteConstants(state, input, expected), "Valid palette rejected");
            EnginePaletteObservation observation;
            require(beginEnginePaletteObservation(base, descriptor, guest.r4.u32, source, observation), "Valid palette observation rejected");
            require(input == observation.input && expected == observation.constants, "Native palette selection differs");
            require(_mm_getcsr() == mode && !std::memcmp(base + fixture, before.data(), extent), "Native palette preparation mutated FP/source");
            auto original = clone(guest); __imp__sub_8224AB18(original, base);
            const auto originalMode = _mm_getcsr();
            const std::vector<uint8_t> after(base + fixture, base + fixture + extent);
            const std::vector<uint8_t> stackAfter(base + stack, base + stack + 4224);
            require(original.r3.u32 == expected.vectorCount, "Original palette count differs");
            require(owner.read32(cursor) == (count ? device + 3456 + count * 48 : 0xBADFADE), "Original palette cursor differs");
            const auto bytes = encodeEngineVertexDescriptor(expected.descriptor);
            require(!std::memcmp(base + descriptor, bytes.data(), 80), "Original palette descriptor differs");
            for (unsigned v = 0; v < expected.vectorCount; ++v) for (unsigned c = 0; c < 4; ++c) {
                require(owner.read32(device + 3456 + v * 16 + c * 4) == expected.vectors[v][c], "Original palette word differs"); ++words;
            }
            finishEnginePaletteObservation(base, original.r3.u32, observation);
            require(observation.comparison == TransformComparison::equal, "Original palette comparison failed");
            require(!std::memcmp(base + fixture, after.data(), extent) && _mm_getcsr() == originalMode, "Finishing palette comparison mutated FP/source");
            for (auto caller : {0x82249110u, 0x12345678u}) {
                std::memcpy(base + fixture, before.data(), extent); std::memcpy(base + stack, stackBefore.data(), stackBefore.size());
                auto wrapped = clone(guest); wrapped.lr = caller; original.lr = caller;
                _mm_setcsr(mode); sub_8224AB18(wrapped, base);
                require(!std::memcmp(&wrapped, &original, sizeof(wrapped)) && !std::memcmp(base + fixture, after.data(), extent) &&
                    !std::memcmp(base + stack, stackAfter.data(), stackAfter.size()) && _mm_getcsr() == originalMode, "Palette wrapper changed complete original ABI"); ++abiCases;
            }
            if (count) {
                const auto retained = observation.input;
                base[device + 3456] ^= 1; finishEnginePaletteObservation(base, expected.vectorCount, observation);
                require(observation.comparison == TransformComparison::different, "Bad palette output reported equal");
                base[matrices] ^= 1; finishEnginePaletteObservation(base, expected.vectorCount, observation);
                require(observation.comparison == TransformComparison::unavailable && observation.input == retained, "Changed palette source was compared or not owned");
            }
            ++cases;
        }
        EnginePaletteInput input; input.requestedCount = 1; EnginePaletteConstants sentinel; sentinel.vectorCount = 999;
        for (auto bad : {0x7F800000u, 0x7FC00000u, 0x7F800001u, 1u, 0x80000001u}) {
            input.selected[0][0][0] = bad;
            require(!buildEnginePaletteConstants({}, input, sentinel) && sentinel.vectorCount == 999, "Unsupported component accepted or changed output");
        }
        put(base, source + 8, 1); put(base, source + 4, 0); put(base, source, 0xFFFFFFF0);
        EnginePaletteObservation invalid; invalid.sourceAddress = 999;
        require(!beginEnginePaletteObservation(base, descriptor, cursor, source, invalid) && invalid.sourceAddress == 999, "Overflowing matrix accepted or changed output");
        put(base, source, matrices); put(base, source + 4, 0xFFFFFFFF);
        require(!beginEnginePaletteObservation(base, descriptor, cursor, source, invalid), "Invalid index pointer accepted");
        require(!beginEnginePaletteObservation(base, descriptor, cursor, 0xFFFFFFF8, invalid), "Overflowing source accepted");
        std::printf("EnginePaletteContract passed: %llu original-AOT cases, %llu bit-exact words, %llu complete wrapper ABI cases; contiguous/indexed selection, duplicate indices, all loop tails, clamp52, empty invalid pointers, opaque descriptor, bounds, ownership and FP state.\n", cases, words, abiCases);
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "EnginePaletteContract failed: %s\n", e.what()); return 1; }
}
