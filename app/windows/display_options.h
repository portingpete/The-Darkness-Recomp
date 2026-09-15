#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>
#include "runtime/native/graphics_settings.h"

struct NativeDisplaySize { uint32_t width, height; };
// Rasterize more pixels on the PC while retaining the console's bounded
// allocation dimensions. Integer scaling keeps tile/resolve edges coincident.
inline uint32_t nativeResolutionScale(NativeDisplaySize render) {
    return (std::max)({1u, (render.height + 719) / 720, (render.width + 2559) / 2560});
}
inline bool parseDisplayDimension(std::wstring_view text, uint32_t& result) {
    if (text.empty()) return false;
    uint32_t number = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || number > 16384 / 10) return false;
        number = number * 10 + uint32_t(c - L'0');
        if (number > 16384) return false;
    }
    if (!number) return false;
    result = number;
    return true;
}
// Keep the established render height unless explicitly requested otherwise.
// The world expands horizontally on wider displays; pixel count stays bounded.
inline NativeDisplaySize renderSizeForDisplay(NativeDisplaySize output, uint32_t requestedHeight = 720) {
    if (!output.width || !output.height || !requestedHeight) return {};
    // Keep host targets bounded; the guest sees these dimensions divided by
    // nativeResolutionScale, with an even logical extent on both axes.
    uint64_t height = (std::min)(requestedHeight, DarkRecomp::Native::kMaximumRenderHeight);
    uint64_t width = (uint64_t(output.width) * height + output.height / 2) / output.height;
    if (width > 4096) { height = height * 4096 / width; width = 4096; }
    for (;;) {
        const uint64_t alignment = 2 * nativeResolutionScale({uint32_t(width), uint32_t(height)});
        width = (std::max)(alignment, width / alignment * alignment);
        height = (std::max)(alignment, height / alignment * alignment);
        if (2 * nativeResolutionScale({uint32_t(width), uint32_t(height)}) == alignment)
            return {uint32_t(width), uint32_t(height)};
    }
}
