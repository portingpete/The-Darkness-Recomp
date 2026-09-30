#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace DarkRecomp::Native {
struct XexImage {
    std::vector<uint8_t> header;
    std::vector<uint8_t> image;
};
// Decode the retail image layout used by the supported disc revision.
// Import thunks remain untouched so the result matches the original image.
XexImage decodeXex(std::span<const uint8_t> bytes);
}
