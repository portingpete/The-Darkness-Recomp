#pragma once

// Include after native_tests.cpp's check(); call after Memory::load/initThread,
// serially with no guest audio workers running (the pool pointer is temporary).
// Tests the original entry, not an override, without opening an audio device.
//
// Original 82820DD8 contract, checked against Darkness/basefile.exe:
//   r3: BE { (sourceChannels << 24) | frames, inputAddress, outputAddress }.
//   r5: BE u32 sourceChannels, u32 destinationChannels, u16 targetHandle.
//   r6: ADDRESS of the BE u16 previousHandle; r4 is unused.
//   Nonzero aligned handles are BYTE OFFSETS from *0x82A695EC, not indices.
//   For 1..4 -> 5..8 channels, the 82821270 path consumes a row-major
//   u16 matrix[8 destination lanes][4 source lanes], scaled by 1/65535.
//   Samples are BE float input[frames][4], output[frames][8]. All padded
//   lanes participate in the dot products; there is no logical-channel mask.
//   Frame n uses previous + n*(target-previous)/frames (old gain at n=0).
//   The target's 64 bytes replace the PREVIOUS MATRIX, not its handle.
//   A null previous handle selects the target immediately, without a fade.
//
// The loop executes 8*ceil(frames/8) frames and dcbzl clears whole 128-byte
// output cache lines. Use positive multiples of eight and disjoint buffers;
// output must be 128-byte aligned, input/matrices at least 16-byte aligned.
// In-place expansion would erase unread input. It is deliberately not tested
// as a supported contract. Zero target handles take a separate bypass path.

#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82820DD8);

static void testAudioVolumeMatrix(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "VolumeMatrix requires loaded Memory and initThread");
    constexpr uint32_t poolGlobal = 0x82A695EC;
    struct Restore {
        Memory* owner;
        PPCContext& context;
        PPCContext saved;
        uint32_t hostCsr;
        uint32_t savedPool;
        uint32_t allocation = 0;
        bool poolInstalled = false;
        Restore(Memory* m, PPCContext& c)
            : owner(m), context(c), hostCsr(c.fpscr.getcsr()),
              savedPool(m->read32(0x82A695EC)) {
            std::memcpy(&saved, &c, sizeof(saved));
        }
        ~Restore() {
            if (poolInstalled) owner->write32(0x82A695EC, savedPool);
            if (allocation) owner->release(allocation);
            std::memcpy(&context, &saved, sizeof(saved));
            context.fpscr.setcsr(hostCsr);
        }
    } restore(memory, ctx);
    auto* const owner = restore.owner;
    auto* const base = owner->base();

    // These words pin the ABI, dispatch, unsigned unpack, history copy, loop
    // width, and cache clearing to the loaded original image. A failure here
    // is an image/contract mismatch, not evidence of a mixer arithmetic bug.
    const std::array<std::array<uint32_t, 2>, 19> instructions{{
        {0x82820DF4, 0x8BBF0000}, // lbz r29,0(r31): source channel byte
        {0x82820DF8, 0x9B9F0000}, // stb r28,0(r31): destination channel byte
        {0x82820E14, 0xA13E0008}, // lhz r9,8(r30): target handle
        {0x82820E18, 0x815F0004}, // input at descriptor+4
        {0x82820E20, 0x817F0008}, // output at descriptor+8
        {0x82820E30, 0x810895EC}, // matrix pool global
        {0x82820E38, 0x7F494214}, // target = pool + unsigned handle
        {0x82820E3C, 0xA13B0000}, // lhz r9,0(r27): r6 is a pointer
        {0x82820EB0, 0x7F49D378}, // missing previous selects target
        {0x82820F78, 0x82820F88}, // one source vector
        {0x82820FB0, 0x82821270}, // two destination vectors
        {0x82821278, 0x1080404C}, // vmrghh with zero: unsigned u16
        {0x82821294, 0x118049C3}, // target -> previous, first 16 bytes
        {0x828212D0, 0x132019C3}, // target -> previous, last 16 bytes
        {0x828213C8, 0x5529E8FE}, // (frames-1) >> 3, outer loop count
        {0x828213E4, 0x39200002}, // two four-frame inner iterations
        {0x828213F8, 0x7C205FEC}, // dcbzl output cache line
        {0x82821400, 0x172011D0}, // vmsum4fp128: four-source dot product
        {0x828215A8, 0x396B0080}, // output += 128 each four frames
    }};
    for (const auto& instruction : instructions)
        check(owner->read32(instruction[0]) == instruction[1],
              "VolumeMatrix original instruction/ABI oracle changed");
    constexpr uint32_t scaleBits = 0x37800080; // float(1/65535), not signed Q15
    for (uint32_t lane = 0; lane < 4; ++lane)
        check(owner->read32(0x82A48EC0 + 4 * lane) == scaleBits,
              "VolumeMatrix original unsigned coefficient scale changed");

    constexpr uint32_t extent = 0x30000, maxFrames = 768;
    constexpr uint32_t inputLanes = 4, outputLanes = 8, destinationChannels = 6;
    constexpr uint32_t matrixBytes = inputLanes * outputLanes * 2;
    restore.allocation = owner->allocate(extent);
    check(restore.allocation != 0, "VolumeMatrix fixture allocation failed");
    const uint32_t block = restore.allocation;
    const uint32_t descriptor = block + 0x100, params = block + 0x140;
    const uint32_t previousHandleCell = block + 0x180, pool = block + 0x1000;
    const uint32_t input = block + 0x2000, output = block + 0x6000;
    const uint32_t stackTop = block + 0x2F000;
    // High-bit handles also detect accidental signed-halfword decoding.
    constexpr uint16_t previousHandle = 0xD200;
    constexpr std::array<uint16_t, 2> targetHandles{0xD400, 0xD600};
    const uint32_t previous = pool + previousHandle;
    check((input & 15) == 0 && (output & 127) == 0 && (previous & 15) == 0 &&
          (stackTop & 15) == 0, "VolumeMatrix fixture alignment is invalid");
    owner->write32(poolGlobal, pool);
    restore.poolInstalled = true;

    using Matrix = std::array<std::array<uint16_t, inputLanes>, outputLanes>;
    using Gains = std::array<std::array<float, inputLanes>, outputLanes>;
    enum class Prior { Separate, Missing, SharedWithTarget };
    auto put16 = [&](uint32_t address, uint16_t value) {
        base[address] = uint8_t(value >> 8);
        base[address + 1] = uint8_t(value);
    };
    auto get16 = [&](uint32_t address) {
        return uint16_t((uint16_t(base[address]) << 8) | base[address + 1]);
    };
    auto putFloat = [&](uint32_t address, float value) {
        owner->write32(address, std::bit_cast<uint32_t>(value));
    };
    auto getFloat = [&](uint32_t address) {
        return std::bit_cast<float>(owner->read32(address));
    };
    auto putMatrix = [&](uint32_t address, const Matrix& values) {
        for (uint32_t dst = 0; dst < outputLanes; ++dst)
            for (uint32_t src = 0; src < inputLanes; ++src)
                put16(address + 2 * (dst * inputLanes + src), values[dst][src]);
    };
    // Scalar matrix arithmetic is independent of VMX shuffles/registers. Float
    // intermediates bound rounding differences; a second, closed-form double
    // oracle below independently checks linear interpolation and block seams.
    auto mul = [](float a, float b) { volatile float x = a * b; return float(x); };
    auto add = [](float a, float b) { volatile float x = a + b; return float(x); };
    auto sub = [](float a, float b) { volatile float x = a - b; return float(x); };
    const float scale = std::bit_cast<float>(scaleBits);
    uint32_t cases = 0, blocks = 0;
    auto run = [&](const char* name, uint32_t sourceChannels,
                   const std::vector<uint32_t>& chunks, const Matrix& initial,
                   const std::vector<Matrix>& targets, Prior priorMode,
                   bool poisonPadding, int basisSource) {
        const std::string label = std::string("VolumeMatrix ") + name + " " +
                                  std::to_string(sourceChannels) + "->6";
        auto require = [&](bool ok, const char* reason) {
            if (!ok) throw std::runtime_error(label + ": " + reason);
        };
        require(sourceChannels == 1 || sourceChannels == 2, "unsupported fixture channels");
        require(!chunks.empty() && chunks.size() == targets.size(), "invalid target schedule");
        uint32_t frames = 0;
        for (uint32_t count : chunks) {
            require(count != 0 && count % 8 == 0 && count <= maxFrames - frames,
                    "fixture blocks must be positive multiples of eight within capacity");
            frames += count;
        }
        std::memset(base + block, 0xA5, extent);
        std::vector<float> signal(frames * inputLanes, 0.0f);
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t src = 0; src < inputLanes; ++src) {
                float sample = 0.0f;
                if (src < sourceChannels) {
                    // Integer/dyadic input avoids platform-dependent libm and
                    // gives every frame/channel a distinct signed sample.
                    const int value = int((f * (37 + 17 * src) + f * f * 3 +
                                           src * 113) % 1021) - 510;
                    sample = float(value) / 2048.0f;
                    if (basisSource >= 0 && src != uint32_t(basisSource)) sample = 0.0f;
                } else if (poisonPadding) {
                    // Zero padded matrix columns must suppress these finite
                    // values. We do not assume the kernel masks padded input.
                    sample = float(11 + src) / 16.0f + float(f % 7) / 64.0f;
                }
                signal[f * inputLanes + src] = sample;
                putFloat(input + 4 * (f * inputLanes + src), sample);
            }
        }
        putMatrix(previous, initial);
        Matrix current = initial;
        uint32_t first = 0;
        double maxError = 0.0, maxIdealError = 0.0, maxSeamError = 0.0;
        for (uint32_t call = 0; call < chunks.size(); ++call) {
            const uint32_t count = chunks[call];
            const Matrix& target = targets[call];
            const uint16_t targetHandle = targetHandles[call % targetHandles.size()];
            const uint32_t targetAddress = pool + targetHandle;
            const uint16_t handle = priorMode == Prior::Missing ? uint16_t(0) :
                priorMode == Prior::SharedWithTarget ? targetHandle : previousHandle;
            const Matrix from = priorMode == Prior::Separate ? current : target;
            putMatrix(targetAddress, target);
            put16(previousHandleCell, handle);
            owner->write32(descriptor, (sourceChannels << 24) | count);
            owner->write32(descriptor + 4, input + first * inputLanes * 4);
            owner->write32(descriptor + 8, output + first * outputLanes * 4);
            owner->write32(params, sourceChannels);
            owner->write32(params + 4, destinationChannels);
            put16(params + 8, targetHandle);

            Gains gain{}, delta{};
            for (uint32_t dst = 0; dst < outputLanes; ++dst) {
                for (uint32_t src = 0; src < inputLanes; ++src) {
                    gain[dst][src] = mul(float(from[dst][src]), scale);
                    delta[dst][src] = mul(sub(mul(float(target[dst][src]), scale),
                                              gain[dst][src]), 1.0f / float(count));
                }
            }
            std::vector<float> expected(count * outputLanes);
            std::vector<double> ideal(count * outputLanes, 0.0);
            for (uint32_t f = 0; f < count; ++f) {
                const double t = double(f) / double(count);
                for (uint32_t dst = 0; dst < outputLanes; ++dst) {
                    std::array<float, inputLanes> products{};
                    for (uint32_t src = 0; src < inputLanes; ++src) {
                        const float sample = signal[(first + f) * inputLanes + src];
                        products[src] = mul(sample, gain[dst][src]);
                        ideal[f * outputLanes + dst] += double(sample) *
                            ((1.0 - t) * double(from[dst][src]) +
                             t * double(target[dst][src])) / 65535.0;
                        gain[dst][src] = add(gain[dst][src], delta[dst][src]);
                    }
                    expected[f * outputLanes + dst] =
                        add(add(products[0], products[1]), add(products[2], products[3]));
                }
            }
            // Snapshot immediately before EVERY call: later calls must not
            // erase earlier output, guards, future output, handles, or input.
            const std::vector<uint8_t> before(base + block, base + block + extent);
            std::memcpy(&ctx, &restore.saved, sizeof(ctx));
            ctx.r1.u64 = stackTop;
            ctx.lr = 0;
            ctx.r3.u64 = descriptor;
            ctx.r4.u64 = 0;
            ctx.r5.u64 = params;
            ctx.r6.u64 = previousHandleCell;
            ctx.fpscr.setcsr((restore.hostCsr & ~PPCFPSCRRegister::RoundMask) |
                            PPCFPSCRRegister::FlushMask);
            ctx.fpscr.loadFromHost();
            for (uint32_t lane = 0; lane < 4; ++lane) {
                ctx.v0.f32[lane] = 71.0f + float(call + lane);
                ctx.v13.f32[lane] = -91.0f - float(call + lane);
                ctx.v31.f32[lane] = 113.0f + float(call + lane);
                ctx.v63.f32[lane] = -137.0f - float(call + lane);
            }
            __imp__sub_82820DD8(ctx, base);
            require(ctx.r1.u32 == stackTop && ctx.lr == 0, "guest stack/LR not restored");
            require(owner->read32(poolGlobal) == pool, "matrix pool global was corrupted");
            require(owner->read32(descriptor) == ((destinationChannels << 24) | count),
                    "descriptor channel/frame result is incorrect");
            require(get16(previousHandleCell) == handle, "previous handle was rewritten");

            const uint32_t destination = output + first * outputLanes * 4;
            for (uint32_t off = 0; off < extent; ++off) {
                const uint32_t address = block + off;
                if (address >= stackTop - 272 && address < stackTop) continue;
                if (address == descriptor) continue; // only the channel byte changes
                if (address >= destination && address < destination + count * outputLanes * 4) continue;
                if (priorMode == Prior::Separate && address >= previous &&
                    address < previous + matrixBytes) continue;
                if (base[address] != before[off]) {
                    std::fprintf(stderr, "%s block=%u unexpected write at %08X: %02X -> %02X\n",
                                 label.c_str(), call, address, unsigned(before[off]), unsigned(base[address]));
                    require(false, "input/target/descriptor/handle/output-boundary/stack guard corrupted");
                }
            }
            if (priorMode == Prior::Separate) {
                for (uint32_t dst = 0; dst < outputLanes; ++dst) {
                    for (uint32_t src = 0; src < inputLanes; ++src) {
                        if (get16(previous + 2 * (dst * inputLanes + src)) != target[dst][src]) {
                            std::fprintf(stderr, "%s block=%u history dst=%u src=%u did not reach target\n",
                                         label.c_str(), call, dst, src);
                            require(false, "target was not copied into previous matrix");
                        }
                    }
                }
            }
            for (uint32_t f = 0; f < count; ++f) {
                for (uint32_t dst = 0; dst < outputLanes; ++dst) {
                    const uint32_t index = f * outputLanes + dst;
                    const float actual = getFloat(destination + index * 4);
                    const double error = std::abs(double(actual) - expected[index]);
                    const double idealError = std::abs(double(actual) - ideal[index]);
                    const double limit = 2.0e-6 * (1.0 + std::abs(double(expected[index])));
                    const double idealLimit = 2.5e-5 * (1.0 + std::abs(ideal[index]));
                    if (!std::isfinite(actual) || error > limit || idealError > idealLimit) {
                        std::fprintf(stderr,
                            "%s block=%u local-frame=%u absolute-frame=%u dst=%u actual=%.9g "
                            "scalar=%.9g linear=%.12g error=%g linear-error=%g\n",
                            label.c_str(), call, f, first + f, dst, double(actual),
                            double(expected[index]), ideal[index], error, idealError);
                        require(false, "original 82821270 mixing/interpolation mismatch; inspect ABI/opcodes before attribution");
                    }
                    // With zero destination padding rows, both padded outputs
                    // must be exactly zero; a broad sample tolerance cannot hide leakage.
                    if (dst >= destinationChannels)
                        require(actual == 0.0f, "padded destination lane is not zero");
                    if (error > maxError) maxError = error;
                    if (idealError > maxIdealError) maxIdealError = idealError;
                    if ((f == 0 || f + 1 == count) && error > maxSeamError) maxSeamError = error;
                }
            }
            // Carry the logical oracle, but NEVER repair/reseed guest history.
            // The next block must consume the bytes written by the original.
            current = target;
            first += count;
            ++blocks;
        }
        std::printf("%s: %zu blocks/%u frames, sample=%g linear=%g seam=%g; guards/history passed.\n",
                    label.c_str(), chunks.size(), frames, maxError, maxIdealError, maxSeamError);
        ++cases;
    };

    for (uint32_t sourceChannels : {1u, 2u}) {
        Matrix a{}, b{}, c{};
        constexpr std::array<std::array<uint16_t, 2>, 6> valuesA{{
            {65535, 0}, {49151, 65535}, {32768, 16384},
            {8192, 49151}, {1, 65534}, {0, 32767}}};
        constexpr std::array<std::array<uint16_t, 2>, 6> valuesB{{
            {0, 65535}, {16384, 32768}, {65535, 8192},
            {49151, 1}, {32767, 0}, {65534, 49151}}};
        constexpr std::array<std::array<uint16_t, 2>, 6> valuesC{{
            {32768, 32767}, {65535, 1}, {1, 65534},
            {0, 16384}, {49151, 32768}, {8192, 65535}}};
        for (uint32_t dst = 0; dst < destinationChannels; ++dst) {
            for (uint32_t src = 0; src < sourceChannels; ++src) {
                a[dst][src] = valuesA[dst][src];
                b[dst][src] = valuesB[dst][src];
                c[dst][src] = valuesC[dst][src];
            }
        }
        run("constant", sourceChannels, {256, 256, 256}, a, {a, a, a}, Prior::Separate, false, -1);
        run("changing/history carry", sourceChannels, {256, 256, 256}, a, {b, c, a}, Prior::Separate, false, -1);
        run("short/non-power-of-two blocks", sourceChannels, {8, 24, 40, 248, 256},
            a, {b, c, a, b, c}, Prior::Separate, false, -1);
        run("poisoned input padding", sourceChannels, {256, 256, 256},
            a, {b, c, a}, Prior::Separate, true, -1);
        run("null previous starts at target", sourceChannels, {8, 256, 256},
            a, {b, c, a}, Prior::Missing, false, -1);
        run("shared matrix handle", sourceChannels, {8, 256, 256},
            a, {b, c, a}, Prior::SharedWithTarget, false, -1);
        for (uint32_t src = 0; src < sourceChannels; ++src)
            run("isolated source basis", sourceChannels, {8, 8, 8},
                a, {a, b, c}, Prior::Separate, false, int(src));
    }
    std::printf("VolumeMatrix original mixing/interpolation/memory contracts: %u cases, %u blocks passed.\n",
                cases, blocks);
}
