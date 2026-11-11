#include "draw_distance.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"

#include <atomic>
#include <bit>

namespace DarkRecomp::Native {
namespace {
std::atomic<unsigned> configuredDrawDistance{0};
}

unsigned drawDistanceSetting() noexcept { return configuredDrawDistance.load(std::memory_order_relaxed); }

bool setDrawDistanceSetting(unsigned units) noexcept {
    if (!validDrawDistance(units)) return false;
    configuredDrawDistance.store(units, std::memory_order_relaxed);
    return true;
}

void applyDrawDistance(uint32_t& flags, float& genericSquared, float& objectSquared, unsigned units) noexcept {
    if (!units || !validDrawDistance(units)) return;
    const float limit = float(units) * float(units);
    auto shorten = [&](uint32_t bit, float& squared) {
        if (!(flags & bit) || !(squared <= limit)) { squared = limit; flags |= bit; }
    };
    shorten(kClientRangeGeneric, genericSquared);
    shorten(kClientRangeObject, objectSquared);
    flags |= kClientRangeAny;
}
}

// 82498C48 fills the client render context (r3) for the current view: enable
// bits at +80 and squared OnClientRender ranges at +88 (generic), +92
// (characters), +96 (engine paths), +100 (objects) and +104 (pickups), copied
// from the view's CWObject_WorldLOD settings. The caller then renders client
// objects with this context. Only the copy is shortened, so no game or save
// state changes and the setting applies from the next frame.
extern "C" PPC_FUNC(__imp__sub_82498C48);
PPC_FUNC(sub_82498C48) {
    const uint32_t context = ctx.r3.u32;
    __imp__sub_82498C48(ctx, base);
    const unsigned units = DarkRecomp::Native::drawDistanceSetting();
    if (!units) return;
    uint32_t flags = PPC_LOAD_U32(context + 80);
    float generic = std::bit_cast<float>(PPC_LOAD_U32(context + 88));
    float object = std::bit_cast<float>(PPC_LOAD_U32(context + 100));
    DarkRecomp::Native::applyDrawDistance(flags, generic, object, units);
    PPC_STORE_U32(context + 80, flags);
    PPC_STORE_U32(context + 88, std::bit_cast<uint32_t>(generic));
    PPC_STORE_U32(context + 100, std::bit_cast<uint32_t>(object));
}
