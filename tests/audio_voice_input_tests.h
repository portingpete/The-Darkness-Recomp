#pragma once

#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_8281E758);

static void testAudioVoiceInput(PPCContext& ctx) {
    using namespace DarkRecomp::Native;
    check(memory && ctx.r1.u32, "VoiceInput requires loaded image and guest stack");
    constexpr uint32_t poolGlobal = 0x82A695EC;
    struct Restore {
        Memory* owner;
        PPCContext& ctx;
        PPCContext saved;
        uint32_t csr;
        std::array<uint8_t, 8> globals{};
        uint32_t allocation = 0;
        ~Restore() {
            std::memcpy(owner->base() + poolGlobal, globals.data(), globals.size());
            ctx = saved;
            ctx.fpscr.setcsr(csr);
            if (allocation) owner->release(allocation);
        }
    } restore{memory, ctx, ctx, ctx.fpscr.getcsr()};
    auto* base = memory->base();
    std::memcpy(restore.globals.data(), base + poolGlobal, restore.globals.size());
    constexpr uint32_t bytes = 0x8000, frames = 2048;
    restore.allocation = memory->allocate(bytes);
    check(restore.allocation, "VoiceInput fixture allocation failed");
    const uint32_t pool = restore.allocation, descriptor = pool + 0x100;
    const uint32_t params = pool + 0x120, state = pool + 0x140;
    const uint32_t voice = pool + 0x400, master = pool + 0x500;
    const uint32_t buffer = pool + 0x600, input = pool + 0x1000, output = pool + 0x5000;
    auto put16 = [&](uint32_t a, uint16_t v) { base[a] = uint8_t(v >> 8); base[a + 1] = uint8_t(v); };
    auto putFloat = [&](uint32_t a, float f) { memory->write32(a, std::bit_cast<uint32_t>(f)); };
    auto getFloat = [&](uint32_t a) { return std::bit_cast<float>(memory->read32(a)); };
    const float scale = std::bit_cast<float>(uint32_t(0x38000100));
    uint32_t cases = 0;
    for (uint32_t channels : {1u, 2u}) {
        for (float rate : {0.5f, 1.0f, 1.25f}) {
            std::memset(base + pool, 0xA5, bytes);
            std::memset(base + descriptor, 0, 0x80);
            std::memset(base + voice, 0, 0x280);
            memory->write32(poolGlobal, pool);
            put16(poolGlobal + 4, 0x500);
            put16(params, 0x400);
            putFloat(params + 4, 0.5f);
            putFloat(state, 0.5f);
            putFloat(master + 60, 1.0f);
            putFloat(voice + 40, rate);
            putFloat(voice + 44, 1.0f);
            putFloat(voice + 48, 0.0f);
            memory->write32(voice + 24, 0);
            memory->write32(voice + 28, frames); // Total source-frame extent (BE64).
            put16(voice + 14, 0x600); // Current buffer, so no allocator/list calls.
            memory->write32(voice + 72, channels << 22);
            memory->write32(buffer + 4, 2u << 30); // Original PCM16 format field.
            memory->write32(buffer + 8, frames << 7);
            memory->write32(buffer + 12, input);
            std::vector<int16_t> samples(frames * channels);
            for (uint32_t n = 0; n < frames; ++n) {
                for (uint32_t c = 0; c < channels; ++c) {
                    const auto value = int16_t(18000.0 * std::sin(n * (0.017 + c * 0.013)) +
                                               4000.0 * std::sin(n * 0.051 + c));
                    samples[n * channels + c] = value;
                    put16(input + (n * channels + c) * 2, uint16_t(value));
                }
            }
            std::vector<uint8_t> before(bytes);
            std::memcpy(before.data(), base + pool, bytes);
            for (uint32_t blockIndex = 0; blockIndex < 3; ++blockIndex) {
                memory->write32(descriptor, (channels << 24) | 256);
                memory->write32(descriptor + 4, 0);
                memory->write32(descriptor + 8, output);
                std::memset(base + output, 0xA5, 4096);
                ctx = restore.saved;
                ctx.r3.u64 = descriptor;
                ctx.r4.u64 = 0;
                ctx.r5.u64 = params;
                ctx.r6.u64 = state;
                __imp__sub_8281E758(ctx, base);
                const std::string label = "VoiceInput ch=" + std::to_string(channels) +
                    " rate=" + std::to_string(rate) + " block=" + std::to_string(blockIndex);
                auto require = [&](bool ok, const std::string& reason) {
                    if (!ok) throw std::runtime_error(label + ": " + reason);
                };
                require(ctx.r1.u32 == restore.saved.r1.u32, "guest stack changed");
                require(memory->read32(descriptor) == ((channels << 24) | 256), "output extent changed");
                require(memory->read32(voice + 52) == uint32_t((blockIndex + 1) * 256 * rate),
                        "buffer cursor progression mismatch");
                require(memory->read32(voice + 32) == 0 &&
                        memory->read32(voice + 36) == memory->read32(voice + 52),
                        "total source cursor progression mismatch");
                require(getFloat(voice + 48) == 0.0f && getFloat(state) == 0.5f,
                        "fractional cursor or gain state changed");
                for (uint32_t n = 0; n < 256; ++n) {
                    const double pos = (blockIndex * 256 + n) * double(rate);
                    const uint32_t i = uint32_t(pos);
                    for (uint32_t c = 0; c < 4; ++c) {
                        double expected = 0.0;
                        if (c < channels) {
                            const double a = samples[i * channels + c];
                            const double b = samples[(i + 1) * channels + c];
                            expected = (a + (b - a) * (pos - i)) * scale * 0.5;
                        }
                        const float actual = getFloat(output + n * 16 + c * 4);
                        require(std::isfinite(actual) && std::abs(actual - expected) < 0.00002,
                                "sample=" + std::to_string(n) + " channel=" + std::to_string(c) +
                                " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
                    }
                }
                for (uint32_t at = 0; at < bytes; ++at) {
                    const uint32_t a = pool + at;
                    if ((a >= descriptor && a < descriptor + 12) ||
                        (a >= state && a < state + 4) ||
                        (a >= voice && a < voice + 80) ||
                        (a >= output && a < output + 4096)) continue;
                    require(base[a] == before[at], "input/guard or unrelated object changed");
                }
            }
            ++cases;
        }
    }
    std::printf("Original VoiceInput continuous three-block fixtures: %u passed\n", cases);
}
