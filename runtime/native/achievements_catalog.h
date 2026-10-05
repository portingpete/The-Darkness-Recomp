#pragma once
#include "achievements.h"
#include <span>

namespace DarkRecomp::Native::Achievements {
// Read only the SPA metadata embedded in the user's verified executable.
// Unknown, truncated or malformed catalogs return an empty list.
// The Russian hint must come from the approved revision identity. It decodes
// that revision's legacy CP1251 game glyphs in the English string-table slot.
std::vector<Entry> parseAchievementCatalog(std::span<const uint8_t> bytes,
                                          uint32_t consoleLanguage,
                                          bool russianEnglishSlot = false);
}
