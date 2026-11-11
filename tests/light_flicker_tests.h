#pragma once
#include "runtime/native/graphics_settings.h"
#include <array>
#include <bit>

extern "C" PPC_FUNC(__imp__sub_822FDF20);

// Exercise the regenerated original light-color evaluator, rather than a
// host-side approximation. All object data and output storage are private.
static void testLightFlicker(PPCContext& ctx) {
    const auto saved = graphicsSettings();
    struct Restore { GraphicsSettings settings; ~Restore() { setGraphicsSettings(settings); } } restore{saved};
    const auto block = memory->allocate(1024);
    check(block != 0, "light fixture allocation failed");
    struct Release { uint32_t block; ~Release() { memory->release(block); } } release{block};
    auto* base = memory->base();
    std::memset(base + block, 0, 1024);
    const auto output = block + 768, time = block + 800;
    auto evaluate = [&](bool reduce, unsigned mode, double seconds,
                        bool enabled = true, unsigned fadeHundredths = 0, uint32_t color = 0x40201000u) {
        auto settings = saved; settings.reduceLightFlicker = reduce;
        check(setGraphicsSettings(settings), "light setting rejected");
        memory->write32(block + 408, color | (mode << 2) | unsigned(enabled));
        memory->write32(block + 420, 0); // Color-transition origin tick.
        memory->write32(block + 428, 0); // No secondary intensity scaling.
        memory->write32(block + 432, (180u << 8) | 128u); // Frequency and depth.
        memory->write32(block + 436, 0x08040200u); // Previous color for fades.
        PPC_STORE_U16(block + 448, uint16_t(fadeHundredths));
        PPC_STORE_U64(time, std::bit_cast<uint64_t>(seconds));
        const std::array<uint8_t, 688> before = [&] {
            std::array<uint8_t, 688> bytes{};
            std::memcpy(bytes.data(), base + block, bytes.size());
            return bytes;
        }();
        auto call = ctx;
        call.r3.u64 = block; call.r4.u64 = output; call.r5.u64 = time;
        call.f1.f64 = 1.0 / 30.0;
        sub_82300818(call, base);
        check(call.r3.u32 == unsigned(enabled), "light enabled state changed");
        check(std::memcmp(before.data(), base + block, before.size()) == 0,
              "light reduction modified serialized light state");
        return std::array{memory->read32(output), memory->read32(output + 4), memory->read32(output + 8)};
    };
    auto originalColor = [&](unsigned mode, double seconds) {
        // Independent original waveform utility, outside the hooked evaluator.
        // The retail evaluator scales the fixture's frequency by 1/8, depth
        // by 1/256 and RGB bytes by the original image's color conversion.
        auto call = ctx;
        call.r6.u64 = mode - 1;
        call.f1.f64 = float(seconds); call.f2.f64 = 180.0f * 0.125f; call.f3.f64 = 128.0f / 256.0f;
        __imp__sub_822FDF20(call, base);
        const float conversion = std::bit_cast<float>(memory->read32(0x8209E6D8));
        auto channel = [&](float byte) { return std::bit_cast<uint32_t>(float(float(byte * conversion) * float(call.f1.f64))); };
        return std::array{channel(64), channel(32), channel(16)};
    };
    bool originalAnimated = false;
    for (unsigned mode = 1; mode <= 5; ++mode) {
        const auto first = evaluate(false, mode, 0.0);
        for (double seconds : {0.0, 0.013, 0.1, 0.37, 1.0, 2.71, 10.0}) {
            const auto normal = originalColor(mode, seconds);
            check(evaluate(false, mode, seconds) == normal,
                  "default setting changed original light animation");
            originalAnimated |= normal != first;
            check(evaluate(true, mode, seconds) == evaluate(false, 0, seconds),
                  "enabled reduction did not produce steady original light color");
            check(evaluate(false, mode, seconds) == normal,
                  "disabling reduction did not immediately restore the original phase");
            check(evaluate(true, mode, seconds, false) == std::array{0u, 0u, 0u},
                  "disabled or destroyed light was relit");
        }
        // Preserve one-shot color transitions, including a fade to black.
        for (uint32_t color : {0u, 0x40201000u})
            for (double seconds : {0.0, 0.2, 0.8, 1.0, 2.0})
                check(evaluate(true, mode, seconds, true, 100, color) ==
                      evaluate(false, 0, seconds, true, 100, color),
                      "flicker reduction changed authored color interpolation");
    }
    check(originalAnimated, "light oracle did not exercise a varying waveform");
    // The shared waveform utility also serves non-light effects. Its direct
    // callers must remain identical with the option On and Off.
    auto waveform = [&](bool reduce) {
        auto settings = saved; settings.reduceLightFlicker = reduce;
        setGraphicsSettings(settings);
        auto call = ctx;
        call.r6.u64 = 1; call.f1.f64 = 0.37; call.f2.f64 = 2.0; call.f3.f64 = 0.5;
        sub_822FDF20(call, base);
        return std::bit_cast<uint64_t>(call.f1.f64);
    };
    check(waveform(false) == waveform(true), "unrelated effect waveform changed");
    puts("Light flicker: original oracle, five steady modes, live restore, disabled lights, fades and state preservation passed.");
}
