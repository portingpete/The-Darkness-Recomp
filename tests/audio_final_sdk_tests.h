#pragma once

// The captured SDK descriptor selects 828D4220, but its PCM16 input is
// 8-byte-offset, so every recorded call actually uses 828CE9B0. Exercise both
// alignments with independent per-channel signals and retained 512-frame input.
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_828C5060);

static void testAudioFinalSdk(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "Final SDK fixture requires loaded image");
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
    constexpr uint32_t extent = 0x18000, frames = 256, channels = 6;
    restore.allocation = memory->allocate(extent);
    check(restore.allocation != 0, "Final SDK fixture allocation failed");
    auto* base = memory->base();
    const uint32_t block = restore.allocation, descriptor = block + 0x100;
    const uint32_t stackLow = block + 0x10000, stackTop = block + 0x17000;
    auto putFloat = [&](uint32_t at, float value) {
        memory->write32(at, std::bit_cast<uint32_t>(value));
    };
    auto getFloat = [&](uint32_t at) {
        return std::bit_cast<float>(memory->read32(at));
    };
    uint32_t cases = 0;
    for (uint32_t alignment : {8u, 0u, 2u}) {
        for (uint32_t flags : {1u, 0x81u}) {
            for (uint32_t pattern = 0; pattern < 3; ++pattern) {
                const std::string label = "Final SDK align=" + std::to_string(alignment) +
                    " flags=" + std::to_string(flags) + " signal=" + std::to_string(pattern);
                auto require = [&](bool ok, const std::string& reason) {
                    if (!ok) throw std::runtime_error(label + ": " + reason);
                };
                std::memset(base + block, 0xA5, extent);
                std::memset(base + descriptor, 0, 88);
                const uint32_t input = block + 0x1000 + alignment;
                const uint32_t output = block + 0x5000;
                memory->write32(descriptor, input);
                memory->write32(descriptor + 4, 512);
                base[descriptor + 12] = 1; // Big-endian signed PCM16 interleaved.
                base[descriptor + 13] = channels;
                memory->write32(descriptor + 16, 48000);
                memory->write32(descriptor + 20, output);
                memory->write32(descriptor + 24, frames);
                memory->write32(descriptor + 32, 48000);
                putFloat(descriptor + 36, 1.0f);
                putFloat(descriptor + 40, 1.0f);
                memory->write32(descriptor + 80, 3);
                memory->write32(descriptor + 84, flags);
                std::array<int16_t, 512 * channels> signal{};
                for (uint32_t call = 0; call < 6; ++call) {
                    // Retain the second half of each 512-frame packet exactly
                    // as in engine07, then replace it only after full consumption.
                    if (call % 2 == 0) {
                        memory->write32(descriptor + 8, 0);
                        for (uint32_t f = 0; f < 512; ++f) {
                            for (uint32_t ch = 0; ch < channels; ++ch) {
                                const uint32_t global = (call / 2) * 512 + f;
                                int value = 0;
                                if (pattern == 0)
                                    value = int((global * (73 + ch * 12) + ch * 5431) % 65536) - 32768;
                                else if (pattern == 1)
                                    value = int(22000.0 * std::sin((global + ch * 13) * (0.009 + ch * 0.003)));
                                else if (f == (ch * 47 + call * 31) % 512)
                                    value = (ch % 2) ? -32768 : 32767;
                                signal[f * channels + ch] = int16_t(value);
                                const uint16_t bits = uint16_t(value);
                                const uint32_t at = input + (f * channels + ch) * 2;
                                base[at] = uint8_t(bits >> 8);
                                base[at + 1] = uint8_t(bits);
                            }
                        }
                    }
                    memory->write32(descriptor + 28, 0);
                    std::memset(base + output, 0xA5, channels * frames * 4);
                    const uint32_t consumed = memory->read32(descriptor + 8);
                    std::vector<uint8_t> before(extent);
                    std::memcpy(before.data(), base + block, extent);
                    ctx = restore.saved;
                    ctx.r1.u64 = stackTop;
                    ctx.r3.u64 = descriptor;
                    __imp__sub_828C5060(ctx, base);
                    require(ctx.r1.u32 == stackTop, "stack not restored");
                    require(ctx.r3.u32 == frames && memory->read32(descriptor + 8) == consumed + frames &&
                            memory->read32(descriptor + 28) == frames, "cursor or return mismatch");
                    require(memory->read32(descriptor + 76) == (flags == 1 ? 0x828D4220u : 0x828CE9B0u) &&
                            memory->read32(descriptor + 80) == 0, "dispatch or dirty-state mismatch");
                    require(getFloat(descriptor + 36) == 1.0f && getFloat(descriptor + 40) == 1.0f &&
                            getFloat(descriptor + 44) == 1.0f && getFloat(descriptor + 48) == 0.0f,
                            "unity gain/rate or phase mismatch");
                    for (uint32_t ch = 0; ch < channels; ++ch) {
                        for (uint32_t f = 0; f < frames; ++f) {
                            const float expected = float(signal[(consumed + f) * channels + ch]) / 32768.0f;
                            const float actual = getFloat(output + (ch * frames + f) * 4);
                            require(actual == expected, "sample mismatch call=" + std::to_string(call) +
                                " channel=" + std::to_string(ch) + " frame=" + std::to_string(f) +
                                " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
                        }
                    }
                    for (uint32_t offset = 0; offset < extent; ++offset) {
                        const uint32_t at = block + offset;
                        if (at >= descriptor && at < descriptor + 88) continue;
                        if (at >= output && at < output + channels * frames * 4) continue;
                        if (at >= stackLow && at < stackTop + 0x100) continue;
                        require(base[at] == before[offset], "input/guard changed at offset=" + std::to_string(offset));
                    }
                    const std::vector<uint8_t> full(base + block, base + block + extent);
                    ctx.r3.u64 = descriptor;
                    __imp__sub_828C5060(ctx, base);
                    require(ctx.r3.u32 == 0, "full destination consumed more input");
                    for (uint32_t offset = 0; offset < extent; ++offset) {
                        const uint32_t at = block + offset;
                        if (at >= stackLow && at < stackTop + 0x100) continue;
                        require(base[at] == full[offset], "full destination modified memory");
                    }
                }
                ++cases;
            }
        }
    }
    std::printf("Final six-channel SDK: %u independent six-block fixtures passed.\n", cases);
}
