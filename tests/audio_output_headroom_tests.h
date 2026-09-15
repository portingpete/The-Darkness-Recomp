#pragma once

#include "runtime/native/audio_output_headroom.h"
#include <array>
#include <limits>

static void testAudioOutputHeadroom() {
    using DarkRecomp::Native::audioOutputHeadroom;
    // Actual matrix read back from the stereo device during the combat capture.
    constexpr std::array<float, 12> stereo{
        1, 0, .707945764f, 0, .707945764f, 0,
        0, 1, .707945764f, 0, 0, .707945764f};
    const float gain = audioOutputHeadroom(stereo, 6);
    check(gain > .413f && gain < .415f, "Stereo fold-down has no headroom");
    // Exhaustive full-scale polarity combinations cover the worst-case sum,
    // including correlated front/rear effects and simultaneous centre dialogue.
    double beforePeak = 0, afterPeak = 0;
    for (unsigned mask = 0; mask < 64; ++mask) for (unsigned row = 0; row < 2; ++row) {
        double sum = 0;
        for (unsigned source = 0; source < 6; ++source)
            sum += stereo[row * 6 + source] * ((mask & (1u << source)) ? .9 : -.9);
        beforePeak = (std::max)(beforePeak, std::abs(sum));
        afterPeak = (std::max)(afterPeak, std::abs(sum * gain));
    }
    check(beforePeak > 2 && afterPeak <= .9, "Simultaneous surround channels overload stereo output");
    std::array<float, 36> surround{};
    for (unsigned channel = 0; channel < 6; ++channel) surround[channel * 6 + channel] = 1;
    check(audioOutputHeadroom(surround, 6) == 1, "One-to-one surround output was attenuated");
    std::array<float, 48> expanded{};
    for (unsigned channel = 0; channel < 6; ++channel) expanded[channel * 6 + channel] = 1;
    check(audioOutputHeadroom(expanded, 6) == 1, "Surround expansion was attenuated");
    constexpr std::array<float, 6> mono{.5f, .5f, .707f, 0, .353f, .353f};
    check(audioOutputHeadroom(mono, 6) < .415f, "Mono fold-down has no headroom");
    constexpr std::array<float, 6> bounded{.25f, -.25f, .25f, 0, .125f, -.125f};
    check(audioOutputHeadroom(bounded, 6) == 1, "Bounded signed mix was attenuated");
    constexpr std::array<float, 4> unequal{1, -1, 0, .5f};
    check(audioOutputHeadroom(unequal, 2) <= .5f, "Signed channel contributions canceled the headroom bound");
    const std::array<float, 2> invalid{1, std::numeric_limits<float>::quiet_NaN()};
    check(audioOutputHeadroom(invalid, 2) == 0 && audioOutputHeadroom(stereo, 0) == 0 &&
          audioOutputHeadroom(stereo, 5) == 0, "Invalid output matrix accepted");
    std::printf("Output headroom: stereo gain=%.9g full-scale peaks %.6f -> %.6f; surround unchanged.\n",
                gain, beforePeak, afterPeak);
}
