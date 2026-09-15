#pragma once
#include <atomic>
#include <cstdint>
#include <cmath>

namespace DarkRecomp::Native {
struct NativeVideoMode { uint32_t width, height; };
inline std::atomic<uint64_t> nativeVideoDimensions{(uint64_t(1280) << 32) | 720};
inline NativeVideoMode nativeVideoMode() {
    const auto packed = nativeVideoDimensions.load(std::memory_order_relaxed);
    return {uint32_t(packed >> 32), uint32_t(packed)};
}
// Configure before starting guest threads. Atomic paired reads also keep test
// queries coherent; live window resizing only changes the host presentation.
inline bool setNativeVideoMode(uint32_t width, uint32_t height) {
    if (width < 320 || height < 180 || width > 4096 || height > 4096) return false;
    nativeVideoDimensions.store((uint64_t(width) << 32) | height, std::memory_order_relaxed);
    return true;
}
bool configureGuestRenderMode(uint32_t display);
bool fitLegacyMenuMatrix(uint32_t drawContext);
inline float fitMenuLabelX(float x, float textWidth, bool rightAligned, float scaleX, NativeVideoMode mode) {
    if (uint64_t(mode.width)*9 <= uint64_t(mode.height)*16 || scaleX <= 0 ||
        !std::isfinite(x) || !std::isfinite(textWidth) || !std::isfinite(scaleX)) return x;
    const float virtualWidth = float(mode.height)*(16.0f/9.0f);
    const float anchorWidth = rightAligned ? textWidth : 0;
    return (x + anchorWidth)*virtualWidth/mode.width - anchorWidth + (mode.width-virtualWidth)*.5f/scaleX;
}
}
