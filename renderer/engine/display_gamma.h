#pragma once
#include <array>
#include <cstdint>

namespace DarkRecomp::Native {
// Completed Xbox display LUT, after D3D's conversion for the output display.
// PWL: 128 base/delta pairs per channel. Normal: 256 entries per channel.
// Interleaved RGB uint2 entries are directly consumable by the presentation GPU.
struct DisplayGamma {
    bool piecewise = true;
    std::array<std::array<uint32_t, 2>, 768> entries{};
    bool operator==(const DisplayGamma&) const = default;
};
}
