#pragma once

// Include after native_tests.cpp's check(), and call after Memory::load/initThread.
// Original BiQuad only: no audio device, decoder, game execution, or global edits.
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82823580);
extern "C" PPC_FUNC(__imp__sub_82845D00);

static void testAudioBiquad(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "BiQuad requires loaded Memory and initThread");
    struct Restore {
        Memory* owner;
        PPCContext& context;
        PPCContext saved;
        uint32_t hostCsr;
        uint32_t allocation = 0;
        Restore(Memory* m, PPCContext& c) : owner(m), context(c), hostCsr(c.fpscr.getcsr()) {
            std::memcpy(&saved, &c, sizeof(saved));
        }
        ~Restore() {
            if (allocation) owner->release(allocation);
            std::memcpy(&context, &saved, sizeof(saved));
            context.fpscr.setcsr(hostCsr);
        }
    } restore(memory, ctx);
    auto* base = memory->base();
    constexpr uint32_t extent = 0x30000, framesPerCall = 256, calls = 3;
    constexpr uint32_t frames = framesPerCall * calls, lanes = 8, channels = 6;
    restore.allocation = memory->allocate(extent);
    check(restore.allocation != 0, "BiQuad fixture allocation failed");
    const uint32_t block = restore.allocation;
    const uint32_t input = block + 0x1000, output = block + 0x8000;
    const uint32_t state = block + 0xF000, previous = state + 256;
    const uint32_t targetAddress = block + 0xF500, coefficientsOut = block + 0xF600;
    const uint32_t stackLow = block + 0x10000, stackTop = block + 0x2F000;

    // The original switch selects ceil(channels/4) == 2, i.e. 0x82823A70.
    // Verify the branch and high-vector instructions used by this fixture.
    const std::array<std::array<uint32_t, 2>, 7> instructions{{
        {0x828236EC, 0x82823A70}, // second entry of original channel-group table
        {0x82823CC4, 0x17E002D4}, // vor128 v63,v0,v0
        {0x82823CC8, 0x17C002D4}, // vor128 v62,v0,v0
        {0x82823D5C, 0x17F7A114}, // vmaddcfp128 v63,v23,v63,v20
        {0x82823D74, 0x17A6D094}, // vmulfp128 v61,v6,v26
        {0x82823E20, 0x13EB29C7}, // stvx128 v63,r11,r5
        {0x82823E24, 0x13CB31C7}, // stvx128 v62,r11,r6
    }};
    for (const auto& instruction : instructions)
        check(memory->read32(instruction[0]) == instruction[1], "BiQuad original instruction oracle changed");

    using Coefficients = std::array<float, 6>; // a1,a2,b0,b1,b2,wet
    using Histories = std::array<std::array<float, lanes>, 4>; // x1,x2,y1,y2
    auto put = [&](uint32_t address, float value) {
        memory->write32(address, std::bit_cast<uint32_t>(value));
    };
    auto get = [&](uint32_t address) {
        return std::bit_cast<float>(memory->read32(address));
    };
    auto putCoefficients = [&](uint32_t address, const Coefficients& values) {
        for (uint32_t i = 0; i < values.size(); ++i) put(address + 4 * i, values[i]);
    };
    auto prepareContext = [&](uint32_t serial) {
        std::memcpy(&ctx, &restore.saved, sizeof(ctx));
        ctx.r1.u64 = stackTop; // All guest stack spills stay in our allocation.
        ctx.lr = 0;
        ctx.fpscr.setcsr((restore.hostCsr & ~PPCFPSCRRegister::RoundMask) | PPCFPSCRRegister::FlushMask);
        ctx.fpscr.loadFromHost();
        for (uint32_t lane = 0; lane < 4; ++lane) {
            ctx.v61.f32[lane] = 101.0f + float(serial + lane);
            ctx.v62.f32[lane] = -211.0f - float(serial + lane);
            ctx.v63.f32[lane] = 307.0f + float(serial + lane);
        }
    };
    // Match emitted separate SIMD multiply/add: volatile intermediates prevent
    // the scalar compiler from contracting these into a fused operation.
    auto mul = [](float a, float b) { volatile float v = a * b; return float(v); };
    auto add = [](float a, float b) { volatile float v = a + b; return float(v); };
    auto sub = [](float a, float b) { volatile float v = a - b; return float(v); };
    auto madd = [&](float a, float b, float c) { return add(mul(a, b), c); };

    auto originalCoefficients = [&](uint32_t type, float hz) {
        std::memset(base + block, 0xA5, extent);
        prepareContext(type);
        ctx.r4.u64 = type; // 0 = low pass; 1 = high pass in the raw jump table.
        ctx.r9.u64 = coefficientsOut;
        ctx.f1.f64 = 48000.0;
        ctx.f2.f64 = hz;
        ctx.f3.f64 = 0.75; // Q
        ctx.f4.f64 = 1.0;  // gain, unused by these two filter types
        ctx.f5.f64 = 1.0;  // wet
        __imp__sub_82845D00(ctx, base);
        Coefficients result{};
        for (uint32_t i = 0; i < result.size(); ++i) result[i] = get(coefficientsOut + 4 * i);

        // Independent RBJ low/high-pass equations check type, signs, and order.
        // Allow coefficient-rounding differences; the recurrence below consumes
        // the actual original float coefficients, essential near unit poles.
        const double omega = 6.2831853071795864769 * hz / 48000.0;
        const double cosine = std::cos(omega), alpha = std::sin(omega) / 1.5;
        const double inverse = 1.0 / (1.0 + alpha);
        const double numerator = type == 1 ? 1.0 + cosine : 1.0 - cosine;
        const std::array<double, 6> expected{
            2.0 * cosine * inverse, -(1.0 - alpha) * inverse,
            0.5 * numerator * inverse, (type == 1 ? -numerator : numerator) * inverse,
            0.5 * numerator * inverse, 1.0};
        for (uint32_t i = 0; i < result.size(); ++i) {
            if (!std::isfinite(result[i]) || std::abs(double(result[i]) - expected[i]) > 2.0e-6) {
                std::fprintf(stderr, "BiQuad coefficient type=%u Hz=%g index=%u actual=%.9g expected=%.12g\n",
                             type, double(hz), i, double(result[i]), expected[i]);
                check(false, "BiQuad original coefficient type/sign/order mismatch");
            }
        }
        return result;
    };

    uint32_t cases = 0;
    auto run = [&](const char* label, Coefficients current, const Coefficients& target,
                   bool inPlace, double tolerance) {
        std::memset(base + block, 0xA5, extent);
        const uint32_t destination = inPlace ? input : output;
        std::vector<float> signal(frames * lanes, 0.0f);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const double t = double(frame) / 48000.0;
                signal[frame * lanes + ch] = float(
                    0.09 * std::sin(6.2831853071795864769 * (37.0 + 43.0 * ch) * t + 0.21 * ch) +
                    0.025 * std::cos(6.2831853071795864769 * (211.0 + 61.0 * ch) * t) +
                    0.003 * (double(ch) - 2.0));
            }
        }
        for (uint32_t i = 0; i < signal.size(); ++i) put(input + 4 * i, signal[i]);
        Histories histories{};
        for (uint32_t bank = 0; bank < 4; ++bank) {
            for (uint32_t ch = 0; ch < lanes; ++ch) {
                // Distinct, exactly representable nonzero histories expose
                // swapped/dropped carry. Both padded lanes remain zero.
                histories[bank][ch] = ch < channels ? float((bank + 1) * (ch + 1)) / 4096.0f : 0.0f;
                put(state + 64 * bank + 4 * ch, histories[bank][ch]);
            }
        }
        putCoefficients(previous, current);
        putCoefficients(targetAddress, target);
        std::vector<uint8_t> before(base + block, base + block + extent);
        double maxSampleError = 0, maxStateError = 0, maxSeamError = 0;
        auto expectNear = [&](float actual, float expected, const char* kind, uint32_t frame, uint32_t lane) {
            const double error = std::abs(double(actual) - expected);
            const double allowed = tolerance * (1.0 + std::abs(double(expected)));
            if (!std::isfinite(actual) || !std::isfinite(expected) || error > allowed) {
                std::fprintf(stderr, "BiQuad %s %s frame=%u lane=%u actual=%.9g expected=%.9g error=%g limit=%g\n",
                             label, kind, frame, lane, double(actual), double(expected), error, allowed);
                throw std::runtime_error(std::string("BiQuad ") + label + ": scalar/state mismatch");
            }
            return error;
        };
        for (uint32_t call = 0; call < calls; ++call) {
            const uint32_t first = call * framesPerCall;
            Coefficients coefficient = current, delta{};
            for (uint32_t i = 0; i < delta.size(); ++i)
                delta[i] = mul(sub(target[i], current[i]), 1.0f / float(framesPerCall));
            std::array<float, framesPerCall * lanes> expected{};
            // Frame n uses old+n*(target-old)/N, then histories advance once.
            for (uint32_t f = 0; f < framesPerCall; ++f) {
                for (uint32_t ch = 0; ch < lanes; ++ch) {
                    const float x = signal[(first + f) * lanes + ch];
                    float y = mul(coefficient[2], x);
                    y = madd(coefficient[3], histories[0][ch], y);
                    y = madd(coefficient[4], histories[1][ch], y);
                    y = madd(coefficient[0], histories[2][ch], y);
                    y = madd(coefficient[1], histories[3][ch], y);
                    // Original vnmsubfp is -(x*wet-x), not (1-wet)*x.
                    expected[f * lanes + ch] = madd(y, coefficient[5], -sub(mul(x, coefficient[5]), x));
                    histories[1][ch] = histories[0][ch]; histories[0][ch] = x;
                    histories[3][ch] = histories[2][ch]; histories[2][ch] = y;
                }
                for (uint32_t i = 0; i < coefficient.size(); ++i)
                    coefficient[i] = add(coefficient[i], delta[i]);
            }
            prepareContext(call + 17 * cases);
            ctx.r3.u64 = destination + first * lanes * 4;
            ctx.r4.u64 = input + first * lanes * 4;
            ctx.r5.u64 = framesPerCall;
            ctx.r6.u64 = (channels + 3) / 4;
            ctx.r7.u64 = previous; ctx.r8.u64 = targetAddress;
            ctx.r9.u64 = state; ctx.r10.u64 = state + 64;
            memory->write32(stackTop + 84, state + 128);
            memory->write32(stackTop + 92, state + 192);
            __imp__sub_82823580(ctx, base);
            for (uint32_t f = 0; f < framesPerCall; ++f) {
                for (uint32_t ch = 0; ch < lanes; ++ch) {
                    const double error = expectNear(get(destination + ((first + f) * lanes + ch) * 4),
                                              expected[f * lanes + ch], "output", first + f, ch);
                    if (error > maxSampleError) maxSampleError = error;
                    if ((f < 2 || f + 2 >= framesPerCall) && error > maxSeamError) maxSeamError = error;
                }
            }
            for (uint32_t bank = 0; bank < 4; ++bank)
                for (uint32_t ch = 0; ch < lanes; ++ch) {
                    const double error = expectNear(get(state + 64 * bank + 4 * ch), histories[bank][ch],
                                              "history", first + framesPerCall, bank * lanes + ch);
                    if (error > maxStateError) maxStateError = error;
                }
            for (uint32_t i = 0; i < current.size(); ++i)
                check(memory->read32(previous + 4 * i) == std::bit_cast<uint32_t>(current[i]),
                      "BiQuad kernel unexpectedly rewrote previous coefficients");
            // Wrapper 828248D0 copies six target words after processing. Keep
            // x1/x2/y1/y2 untouched across all three 256-frame calls.
            current = target;
            putCoefficients(previous, current);
        }
        for (uint32_t off = 0; off < extent; ++off) {
            const uint32_t address = block + off;
            if (address >= stackLow) continue;
            if (address >= destination && address < destination + frames * lanes * 4) continue;
            if (address >= previous && address < previous + 24) continue;
            if (address >= state && address < state + 256 && (address - state) % 64 < lanes * 4) continue;
            check(base[address] == before[off], "BiQuad changed input, guards, target coefficients, or unused state lanes");
        }
        std::printf("BiQuad %s: 6ch/8 padded, 3x256, sample=%g state=%g seam=%g max errors.\n",
                    label, maxSampleError, maxStateError, maxSeamError);
        ++cases;
    };

    const Coefficients stable{0.25f, -0.125f, 0.5f, 0.125f, 0.0625f, 0.75f};
    const Coefficients ramp{0.375f, -0.0625f, 0.25f, 0.25f, 0.125f, 0.5f};
    prepareContext(0);
    run("dyadic fixed separate", stable, stable, false, 2.0e-7);
    run("dyadic fixed in-place", stable, stable, true, 2.0e-7);
    run("dyadic coefficient/wet ramp", stable, ramp, true, 2.0e-7);
    Coefficients dry = stable; dry[5] = 0;
    run("dry output with live filter histories", dry, dry, false, 2.0e-7);
    const auto hp20 = originalCoefficients(1, 20.0f);
    const auto lp50 = originalCoefficients(0, 50.0f);
    const auto hp50 = originalCoefficients(1, 50.0f);
    run("original HP20 Q=.75", hp20, hp20, true, 2.0e-4);
    run("original LP50 Q=.75", lp50, lp50, false, 2.0e-4);
    run("original HP50 Q=.75", hp50, hp50, true, 2.0e-4);
    std::printf("BiQuad original coefficient/recurrence/state-carry contracts: %u cases passed.\n", cases);
}
