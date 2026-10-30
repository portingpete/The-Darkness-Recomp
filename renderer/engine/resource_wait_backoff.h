#pragma once
#include <cstdint>

// Rust owns a wake channel only. All completion checks stay in guest code.
struct DarkResourceWaitToken { uint32_t generation, slot; };
static_assert(sizeof(DarkResourceWaitToken) == 8);
extern "C" uint32_t dark_resource_wait_register();
extern "C" uintptr_t dark_resource_wait_begin(DarkResourceWaitToken*, uint32_t slot);
extern "C" uint32_t dark_resource_wait_arm(const DarkResourceWaitToken*);
extern "C" void dark_resource_wait_finish(DarkResourceWaitToken*);
extern "C" void dark_resource_wait_notify();

constexpr bool resourceDependencyYieldEligible(uint32_t caller, uint32_t milliseconds,
    uint32_t alertable, uint32_t pcr, uint32_t queue, uint32_t record) noexcept {
    return caller == 0x8222CD30u && milliseconds == 1 && alertable == 0 &&
        pcr != 0 && queue != 0 && record != 0;
}
