#pragma once
#include "display_mode.h"
#include <optional>

namespace DarkRecomp::Native {
struct FlashbackTransform { float scaleX, translationX; };
struct FlashbackLayout {
    FlashbackTransform image, leftBar, rightBar;
    float marginPixels;
    uint32_t matteColor;
};

// Game message 0x160's mode 2 paints the authored 853x480 flashback canvas
// directly through the height-scaled HUD. Its square source texture is already
// mapped to that canvas; retain that mapping and fit the displayed rectangle.
inline std::optional<FlashbackLayout> fitFlashbackLayout(
    uint32_t imageMode, uint32_t authoredDimensions, NativeVideoMode viewport,
    float logicalScaleX, float logicalScaleY, FlashbackTransform original,
    float rectLeft, float rectWidth, float rectHeight, uint32_t imageColor) noexcept {
    if (imageMode != 2 || authoredDimensions != ((853u << 16) | 480u) ||
        uint64_t(viewport.width) * 9 <= uint64_t(viewport.height) * 16 ||
        !viewport.height || !std::isfinite(logicalScaleX) || !std::isfinite(logicalScaleY) ||
        !std::isfinite(original.scaleX) || !std::isfinite(original.translationX) ||
        !std::isfinite(rectLeft) || rectWidth != 853 || rectHeight != 480 ||
        original.scaleX <= 0 || logicalScaleX <= 0 ||
        std::abs(logicalScaleX - float(viewport.height) / 480) > .0001f ||
        std::abs(logicalScaleY - logicalScaleX) > .0001f) return std::nullopt;

    const float imageWidth = rectWidth * logicalScaleX;
    const float margin = (float(viewport.width) - imageWidth) * .5f;
    if (!std::isfinite(margin) || margin <= 0) return std::nullopt;
    const float perPixel = original.scaleX / logicalScaleX;
    const float barScale = margin * perPixel / rectWidth;
    // The bars reuse the original (integer-quantized) rectangle. Account for
    // its origin when changing only the clone's horizontal transform.
    const float barOrigin = original.translationX + rectLeft * (original.scaleX - barScale);
    const float imageTranslation = original.translationX + margin * perPixel;
    const float rightTranslation = barOrigin + (float(viewport.width) - margin) * perPixel;
    if (!std::isfinite(barScale) || !std::isfinite(barOrigin) ||
        !std::isfinite(imageTranslation) || !std::isfinite(rightTranslation)) return std::nullopt;
    return FlashbackLayout{
        {original.scaleX, imageTranslation},
        {barScale, barOrigin},
        {barScale, rightTranslation},
        margin, imageColor & 0xff000000u};
}
}
