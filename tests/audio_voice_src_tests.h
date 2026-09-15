#pragma once

// The game's per-voice SRC is separate from the SDK's final six-channel SRC.
// Exercise the original dispatch and VMX kernels against independent sample
// interpolation, including calls split across input/output buffer boundaries.
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_8281D758);
extern "C" PPC_FUNC(__imp__sub_8281DB98);

static void testAudioVoiceSrc(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "Voice SRC requires loaded image and guest stack");
    struct Restore {
        Memory* owner;
        PPCContext& ctx;
        PPCContext saved;
        uint32_t csr;
        uint32_t allocation = 0;
        ~Restore() {
            ctx = saved;
            ctx.fpscr.setcsr(csr);
            if (allocation) owner->release(allocation);
        }
    } restore{memory, ctx, ctx, ctx.fpscr.getcsr()};
    constexpr uint32_t bytes = 0x8000;
    restore.allocation = memory->allocate(bytes);
    check(restore.allocation != 0, "Voice SRC fixture allocation failed");
    auto* base = memory->base();
    const uint32_t block = restore.allocation;
    const uint32_t countSlot = block + 0x100;
    const uint32_t output = block + 0x5000;
    constexpr uint32_t sourceFrames = 1024, outputFrames = 256;
    check(memory->read32(0x82A47C30) == 0x38000100,
          "Voice SRC original signed PCM scale changed");
    check(memory->read32(0x828330E8) == 0x90E1FE20 &&
          memory->read32(0x82833474) == 0x80C1FE20 &&
          memory->read32(0x82833478) == 0x556A0030 &&
          memory->read32(0x82833480) == 0x40990014 &&
          memory->read32(0x8283348C) == 0x7C2A3FEC,
          "Voice SRC original cache-line tracking oracle changed");
    const float scale = std::bit_cast<float>(uint32_t(0x38000100));
    uint32_t cases = 0, numericalFailures = 0;
    auto run = [&](uint32_t channels, float rate, float phase, bool unitPath,
                   bool ramp, uint32_t offset, const std::vector<uint32_t>& chunks,
                   bool advanceInput) {
        const std::string label = "Voice SRC ch=" + std::to_string(channels) +
            " rate=" + std::to_string(rate) + " phase=" + std::to_string(phase) +
            " unit=" + std::to_string(unitPath) + " ramp=" + std::to_string(ramp) +
            " offset=" + std::to_string(offset) + " chunks=" + std::to_string(chunks.size()) +
            " advance=" + std::to_string(advanceInput);
        auto require = [&](bool ok, const std::string& reason) {
            if (!ok) throw std::runtime_error(label + ": " + reason);
        };
        uint32_t caseFailures = 0;
        const bool originalOddTail = unitPath && chunks == std::vector<uint32_t>{127, 129};
        auto expectNear = [&](double actual, double expected, const std::string& reason) {
            if (std::isfinite(actual) && std::abs(actual - expected) < 0.00002) return;
            if (caseFailures++ < 3)
                std::fprintf(stderr, "%s: %s actual=%.9g expected=%.9g\n",
                             label.c_str(), reason.c_str(), actual, expected);
            ++numericalFailures;
        };
        std::memset(base + block, 0xA5, bytes);
        const uint32_t input = block + 0x1000 + offset;
        std::vector<int16_t> samples(sourceFrames * channels);
        for (uint32_t frame = 0; frame < sourceFrames; ++frame) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                // Distinct, nonperiodic channel data catches packing, shuffle,
                // sign-extension, and fractional interpolation mistakes.
                const int value = int((frame * (173 + ch * 82) + frame * frame * 3 +
                                       ch * 1877) % 48001) - 24000;
                const auto sample = int16_t(value);
                samples[frame * channels + ch] = sample;
                const uint32_t address = input + (frame * channels + ch) * 2;
                base[address] = uint8_t(uint16_t(sample) >> 8);
                base[address + 1] = uint8_t(sample);
            }
        }
        std::vector<uint8_t> before(bytes);
        std::memcpy(before.data(), base + block, bytes);
        uint32_t produced = 0, consumed = 0;
        float position = phase;
        const float delta = ramp ? 1.0f / 1024.0f : 0.0f;
        for (uint32_t requested : chunks) {
            require(requested && produced + requested <= outputFrames, "invalid chunk");
            memory->write32(countSlot, requested);
            ctx = restore.saved;
            ctx.r3.u64 = channels;
            ctx.r4.u64 = input + consumed * channels * 2;
            ctx.r5.u64 = sourceFrames - consumed;
            ctx.f1.f64 = position;
            ctx.f2.f64 = rate;
            for (uint32_t lane = 0; lane < 4; ++lane) {
                ctx.v1.f32[3 - lane] = 0.25f + lane * 0.125f + produced * delta;
                ctx.v2.f32[3 - lane] = delta;
            }
            if (unitPath) {
                ctx.r7.u64 = output + produced * 16;
                ctx.r8.u64 = countSlot;
                __imp__sub_8281DB98(ctx, base);
            } else {
                ctx.r8.u64 = output + produced * 16;
                ctx.r9.u64 = countSlot;
                __imp__sub_8281D758(ctx, base);
            }
            require(memory->read32(countSlot) == requested, "output progress changed");
            expectNear(ctx.f1.f64, position + requested * rate, "returned source position");
            require(ctx.r1.u32 == restore.saved.r1.u32, "guest stack not restored");
            position = float(ctx.f1.f64);
            if (advanceInput) {
                const uint32_t used = uint32_t(position);
                consumed += used;
                position -= float(used);
            }
            produced += requested;
        }
        require(produced == outputFrames, "incomplete fixture output");
        for (uint32_t frame = 0; frame < outputFrames; ++frame) {
            const double sourcePosition = double(phase) + frame * double(rate);
            const uint32_t index = uint32_t(sourcePosition);
            const double fraction = unitPath ? 0.0 : sourcePosition - index;
            for (uint32_t lane = 0; lane < 4; ++lane) {
                double expected = 0.0;
                if (lane < channels) {
                    const double a = samples[index * channels + lane];
                    const double b = samples[(index + 1) * channels + lane];
                    expected = (a + (b - a) * fraction) * scale *
                        (0.25 + lane * 0.125 + frame * double(delta));
                }
                // Characterize an original unit-kernel defect separately:
                // bulk prezeroing tracks only its first cache line, so an odd
                // output start followed by a scalar tail zeros frames 248..254
                // before writing frame 255. This is not a production fix or
                // evidence that gameplay exercises the affected split.
                if (originalOddTail && frame >= 248 && frame < 255) expected = 0.0;
                const float actual = std::bit_cast<float>(memory->read32(output + frame * 16 + lane * 4));
                expectNear(actual, expected, "sample=" + std::to_string(frame) + " lane=" + std::to_string(lane));
            }
        }
        for (uint32_t at = 0; at < bytes; ++at) {
            const uint32_t address = block + at;
            if (address >= countSlot && address < countSlot + 4) continue;
            if (address >= output && address < output + outputFrames * 16) continue;
            require(base[address] == before[at], "input/guard changed offset=" + std::to_string(at));
        }
        ++cases;
    };
    for (uint32_t channels : {1u, 2u}) {
        for (uint32_t offset : {0u, 2u}) {
            // Unit-rate optimized kernels use a constant gain for their bulk
            // loop. Exercise that supported contract separately from the
            // variable-rate interpolation kernels' per-sample gain ramp.
            run(channels, 1.0f, 0.0f, true, false, offset, {256}, false);
            run(channels, 1.0f, 0.0f, true, false, offset, {128, 128}, true);
            run(channels, 1.0f, 0.0f, true, false, offset, {127, 129}, true);
            for (float rate : {0.5f, 0.75f, 1.25f, 2.0f}) {
                for (float phase : {0.0f, 0.25f, 0.75f}) {
                    for (bool ramp : {false, true}) {
                        run(channels, rate, phase, false, ramp, offset, {256}, false);
                        run(channels, rate, phase, false, ramp, offset, {127, 129}, true);
                    }
                }
            }
        }
    }
    // Cover every output cache-line alignment at a source split. The live
    // variable-rate path can stop after any frame when an input block ends.
    for (uint32_t channels : {1u, 2u}) {
        for (uint32_t split = 1; split < outputFrames; ++split) {
            for (float rate : {0.75f, 1.25f}) {
                run(channels, rate, 0.25f, false, false, 2,
                    {split, outputFrames - split}, true);
            }
        }
    }
    std::printf("AudioVoiceSrc original interpolation/continuity cases: %u; sample mismatches: %u\n",
                cases, numericalFailures);
    check(numericalFailures == 0, "Voice SRC sample continuity mismatch");
}
