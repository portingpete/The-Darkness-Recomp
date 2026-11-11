#pragma once
#include <cstdint>

namespace DarkRecomp::Native {
// Client draw distance, in world units. Zero (Original) keeps the game's own
// ranges, which most levels leave unlimited. Otherwise generic client-rendered
// objects and physics objects farther than this from the camera are not drawn;
// a level's own shorter range still applies. Characters, pickups and engine
// paths always keep the game's ranges. Settings without a saved value use
// kDefaultDrawDistance (Near), which roughly halves world draws in large open
// views such as the Chapter 2 village seen from above.
inline constexpr unsigned kMaximumDrawDistance = 65535;
inline constexpr unsigned kDefaultDrawDistance = 1500;
constexpr bool validDrawDistance(unsigned units) noexcept { return units <= kMaximumDrawDistance; }
unsigned drawDistanceSetting() noexcept;
bool setDrawDistanceSetting(unsigned units) noexcept;

// The original client render context (82498C48) holds one enable bit and one
// squared range per object category. Generic objects use bit 0x10 and the
// first range, physics objects bit 0x02 and the fourth; 0x20 means any range.
inline constexpr uint32_t kClientRangeGeneric = 0x10, kClientRangeObject = 0x02, kClientRangeAny = 0x20;
// Pure: shorten the generic and object ranges to `units` (squared), keeping a
// shorter enabled range. Zero leaves everything unchanged.
void applyDrawDistance(uint32_t& flags, float& genericSquared, float& objectSquared, unsigned units) noexcept;
}
