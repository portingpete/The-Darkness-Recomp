#pragma once
#include "runtime/native/timebase_scale.h"
#include <array>
#include <limits>

static void testTimebaseScale() {
    using Scale = TimebaseScale<kTimebaseFrequency>;
    constexpr auto maximum = (std::numeric_limits<uint64_t>::max)();
    constexpr Scale common(10000000);
    static_assert(common(10000000) == kTimebaseFrequency && common(80) == 399);
    // This is the prior production expression, independently of the reduced
    // ratio and integral-multiplier paths in the helper under test.
    auto reference = [](uint64_t ticks, uint64_t frequency) {
        return ticks / frequency * kTimebaseFrequency +
            ticks % frequency * kTimebaseFrequency / frequency;
    };
    constexpr std::array frequencies{uint64_t(1), uint64_t(125000), uint64_t(1000000),
        uint64_t(10000000), kTimebaseFrequency, uint64_t(3000000), uint64_t(9999999),
        uint64_t(3579545), uint64_t(123456789), maximum};
    uint64_t comparisons = 0;
    for (uint64_t frequency : frequencies) {
        const Scale scale(frequency);
        auto compare = [&](uint64_t ticks) {
            const auto expected = reference(ticks, frequency), actual = scale(ticks);
            if (actual != expected)
                std::fprintf(stderr, "Timebase ticks=%llu frequency=%llu expected=%llu actual=%llu\n",
                             ticks, frequency, expected, actual);
            check(actual == expected, "Timebase conversion changed exact integer output");
            ++comparisons;
        };
        for (uint64_t ticks : {uint64_t(0), uint64_t(1), uint64_t(79), uint64_t(80), uint64_t(81),
            uint64_t(399), uint64_t(9999999), uint64_t(10000000), uint64_t(10000001),
            maximum / 399, maximum / kTimebaseFrequency, maximum - 1, maximum}) compare(ticks);
        // Exercise both quotient boundaries and all reduced-ratio residues at
        // ordinary and wrapped output positions. Input generation is uint64.
        for (uint64_t quotient : {uint64_t(0), uint64_t(1), maximum / kTimebaseFrequency,
                                   maximum / kTimebaseFrequency + 1, maximum / 399, maximum}) {
            for (uint64_t unit : {uint64_t(80), frequency}) {
                const auto boundary = quotient * unit;
                compare(boundary - 1); compare(boundary); compare(boundary + 1);
            }
            if (frequency == 10000000)
                for (uint64_t residue = 0; residue < 80; ++residue) compare(quotient * 80 + residue);
        }
        uint64_t random = 0x9e3779b97f4a7c15ull ^ frequency;
        for (unsigned i = 0; i < 200000; ++i) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            compare(random);
        }
    }
    LARGE_INTEGER frequency{};
    check(QueryPerformanceFrequency(&frequency) != 0 && frequency.QuadPart > 0,
          "Host timebase frequency unavailable");
    for (unsigned i = 0; i < 1000; ++i) {
        LARGE_INTEGER before{}, after{};
        QueryPerformanceCounter(&before);
        const auto actual = PPCQueryTimebase();
        QueryPerformanceCounter(&after);
        check(actual >= reference(uint64_t(before.QuadPart), uint64_t(frequency.QuadPart)) &&
              actual <= reference(uint64_t(after.QuadPart), uint64_t(frequency.QuadPart)),
              "Production timebase lies outside the original QPC conversion bracket");
    }
    std::printf("Timebase exact scaling: %llu edge/random comparisons and 1000 production QPC brackets.\n",
                comparisons);
}
