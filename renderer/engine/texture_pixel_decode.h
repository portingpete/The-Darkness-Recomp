#pragma once
#include <array>
#include <cstdint>

namespace DarkRecomp {
// Xenos format 3 (1_5_5_5) stores R in bits 0..4, G in 5..9,
// B in 10..14 and A in 15. Input has already undergone fetch-endian
// conversion; component selectors are applied after this unpacking.
// https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/shaders/pixel_formats.xesli
inline std::array<uint8_t,4> decodeTexture1555(const uint8_t* bytes) noexcept {
    const uint16_t packed=uint16_t(bytes[0])|(uint16_t(bytes[1])<<8);
    const auto unorm=[](unsigned value) {return uint8_t((value*255+15)/31);};
    return {unorm(packed&31),unorm((packed>>5)&31),unorm((packed>>10)&31),
            uint8_t(packed&0x8000?255:0)};
}
}
