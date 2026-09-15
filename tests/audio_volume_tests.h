#pragma once

// Exercise the game's scalar and per-channel volume DSPs against independent
// linear ramps. Include after native_tests.cpp's check(), with a loaded image.
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

extern "C" PPC_FUNC(__imp__sub_82822988);
extern "C" PPC_FUNC(__imp__sub_82828020);

static void testAudioVolume(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "Volume DSP requires a loaded image and guest stack");
    struct Restore {
        Memory* owner;
        PPCContext& context;
        PPCContext saved;
        uint32_t csr;
        uint32_t allocation = 0;
        ~Restore() {
            context = saved;
            context.fpscr.setcsr(csr);
            if (allocation) owner->release(allocation);
        }
    } restore{memory, ctx, ctx, ctx.fpscr.getcsr()};
    constexpr uint32_t extent = 0x20000, calls = 3;
    restore.allocation = memory->allocate(extent);
    check(restore.allocation != 0, "Volume DSP fixture allocation failed");
    auto* base = memory->base();
    const uint32_t block = restore.allocation;
    const uint32_t descriptor = block + 0x100, parameters = block + 0x200;
    const uint32_t state = block + 0x300, stackLow = block + 0x10000;
    const uint32_t stackTop = block + 0x1E000;
    auto put = [&](uint32_t at, float value) {
        memory->write32(at, std::bit_cast<uint32_t>(value));
    };
    auto get = [&](uint32_t at) {
        return std::bit_cast<float>(memory->read32(at));
    };
    uint32_t cases = 0;
    for (bool perChannel : {false, true}) {
        for (uint32_t channels : {1u, 2u, 4u, 6u, 8u, 12u, 16u}) {
            const uint32_t lanes = ((channels + 3) / 4) * 4;
            for (uint32_t frames : {8u, 64u, 256u}) {
                for (bool ramp : {false, true}) {
                    const std::string label = std::string(perChannel ? "VolumeMultiChn" : "Volume") +
                        " ch=" + std::to_string(channels) + " frames=" + std::to_string(frames) +
                        " ramp=" + std::to_string(ramp);
                    auto require = [&](bool ok, const std::string& reason) {
                        if (!ok) throw std::runtime_error(label + ": " + reason);
                    };
                    std::memset(base + block, 0xA5, extent);
                    // A vector-aligned but non-cache-line-aligned buffer also
                    // catches accidental assumptions about cache-line starts.
                    const uint32_t input = block + 0x1010;
                    const uint32_t samples = calls * frames * lanes;
                    std::vector<float> signal(samples);
                    for (uint32_t f = 0; f < calls * frames; ++f) {
                        for (uint32_t ch = 0; ch < lanes; ++ch) {
                            const float value = ch < channels ?
                                float(int((f * (37 + 2 * ch) + ch * 113) % 997) - 498) / 2048.0f : 0.0f;
                            signal[f * lanes + ch] = value;
                            put(input + (f * lanes + ch) * 4, value);
                        }
                    }
                    const uint32_t gains = perChannel ? 16u : 1u;
                    std::array<float, 16> old{};
                    for (uint32_t ch = 0; ch < gains; ++ch) {
                        old[ch] = 0.125f + float(ch) / 32.0f;
                        put(state + ch * 4, old[ch]);
                    }
                    for (uint32_t call = 0; call < calls; ++call) {
                        std::array<float, 16> target{};
                        for (uint32_t ch = 0; ch < gains; ++ch) {
                            target[ch] = !ramp ? old[ch] :
                                (call == 0 ? 0.75f - float(ch) / 64.0f :
                                 call == 1 ? 0.0625f + float(ch) / 64.0f : 0.5f);
                            put(parameters + ch * 4, target[ch]);
                        }
                        const uint32_t currentInput = input + call * frames * lanes * 4;
                        memory->write32(descriptor, (channels << 24) | frames);
                        memory->write32(descriptor + 4, currentInput);
                        memory->write32(descriptor + 8, block + 0xE000);
                        std::vector<uint8_t> before(extent);
                        std::memcpy(before.data(), base + block, extent);
                        ctx = restore.saved;
                        ctx.r1.u64 = stackTop;
                        ctx.r3.u64 = descriptor;
                        ctx.r5.u64 = parameters;
                        ctx.r6.u64 = state;
                        if (perChannel) __imp__sub_82828020(ctx, base);
                        else __imp__sub_82822988(ctx, base);
                        require(ctx.r1.u32 == stackTop, "guest stack was not restored");
                        for (uint32_t f = 0; f < frames; ++f) {
                            for (uint32_t ch = 0; ch < lanes; ++ch) {
                                const uint32_t gain = perChannel ? ch : 0;
                                const double scale = double(old[gain]) +
                                    (double(target[gain]) - old[gain]) * double(f) / frames;
                                const double expected = double(signal[(call * frames + f) * lanes + ch]) * scale;
                                const float actual = get(currentInput + (f * lanes + ch) * 4);
                                if (!std::isfinite(actual) || std::abs(double(actual) - expected) > 2e-6) {
                                    std::fprintf(stderr, "%s call=%u frame=%u channel=%u actual=%.9g expected=%.12g\n",
                                                 label.c_str(), call, f, ch, double(actual), expected);
                                    require(false, "independent linear-ramp sample mismatch");
                                }
                            }
                        }
                        for (uint32_t ch = 0; ch < gains; ++ch)
                            require(get(state + ch * 4) == target[ch], "next-block gain state mismatch");
                        for (uint32_t offset = 0; offset < extent; ++offset) {
                            const uint32_t at = block + offset;
                            if (at >= currentInput && at < currentInput + frames * lanes * 4) continue;
                            if (at >= state && at < state + gains * 4) continue;
                            if (at >= stackLow && at < stackTop + 0x100) continue;
                            require(base[at] == before[offset], "input/parameter/guard changed at offset=" + std::to_string(offset));
                        }
                        old = target;
                    }
                    ++cases;
                }
            }
        }
    }
    std::printf("Volume DSP: %u independent three-block scalar/channel ramp fixtures passed.\n", cases);
}
