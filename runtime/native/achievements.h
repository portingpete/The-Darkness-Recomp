#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DarkRecomp::Native::Achievements {
struct Entry {
    uint32_t id = 0, gamerscore = 0, imageId = 0, flags = 0;
    std::string name, description, lockedDescription;
    // UTC Windows FILETIME, zero while locked. IDs come from the game's XDBF.
    uint64_t unlockedAt = 0;
};
struct Snapshot {
    std::vector<Entry> entries;
    std::string error;
};
void initialize(std::span<const uint8_t> xdbf, uint32_t language, bool russianEnglishSlot = false);
void reset();
Snapshot snapshot();
// Writes the entire batch atomically before publishing any new unlocks.
uint32_t award(std::span<const uint32_t> ids);
uint32_t requestShow(uint32_t user);
bool consumeShowRequest();
void setViewerOpen(bool open);
bool viewerOpen();
}
