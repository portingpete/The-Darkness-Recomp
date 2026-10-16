#pragma once
#include "simple_mesh.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace DarkRecomp::Native {
// Western glyphs and the fingerprinted Russian ordinary TEXT font may be
// replaced. Decorative and unknown fonts retain their authored glyphs. Read the
// original CStr before the font loader consumes it.
bool subtitleFontResourceAllowsReplacement(uint8_t* base, uint32_t resource) noexcept;
// TEXT uses Latin-1 and Windows-1252 slots, including Unicode punctuation.
// Unknown/custom slots retain their authored coverage.
std::optional<uint32_t> westernSubtitleCodepoint(uint16_t slot) noexcept;
// Only used after matching the approved Russian TEXT metadata and atlas.
// CP1251 letters become Unicode; custom controller-symbol slots stay authored.
std::optional<uint32_t> russianSubtitleCodepoint(uint16_t slot) noexcept;
// Snapshot the completed definition, including loads before the lazy upload.
// A rejected load also invalidates an earlier definition at the reused address.
void registerSubtitleFont(uint8_t* base, uint32_t font, bool replaceable) noexcept;
// A completed font load can precede its lazy atlas upload. Verified subtitle
// producers may retry against their current font and captured owned generation.
void ensureSubtitleFontRegistered(uint8_t* base, uint32_t font) noexcept;
// Size controls geometry; this factor controls only vector rasterization.
unsigned subtitleFontRasterScale(unsigned renderHeight, unsigned subtitlePercent) noexcept;
// A missing/unsupported definition or failed rasterization keeps the original.
// Registration binds to the captured owned image generation and rejects ID reuse.
std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& source, unsigned factor) noexcept;
std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t textureId,
    const std::shared_ptr<const AlphaImage>& source, unsigned factor) noexcept;
}
