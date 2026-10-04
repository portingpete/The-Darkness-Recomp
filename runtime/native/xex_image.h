#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace DarkRecomp::Native {
struct GameRevision {
    const char* xexSha256;
    const char* imageSha256;
    const char* name;
};
struct XexImage {
    std::vector<uint8_t> header;
    std::vector<uint8_t> image;
    const char* revisionName = nullptr;
};
// Decode the retail image layout used by the supported disc revision.
// Import thunks remain untouched so the result matches the original image.
XexImage decodeXex(std::span<const uint8_t> bytes);
// Accept only an approved file and its paired decoded image. Preserve the
// revision's localization data; never substitute the image used to build AOT.
XexImage decodeVerifiedXex(std::span<const uint8_t> bytes, std::span<const GameRevision> revisions);
}
