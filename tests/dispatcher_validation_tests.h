#pragma once
#include "renderer/engine/engine_performance.h"

static void testDispatcherValidation() {
    auto* base = memory->base();
    struct Allocation {
        uint32_t address = memory->allocate(0x3000);
        ~Allocation() {
            if (!address) return;
            DWORD previous;
            VirtualProtect(memory->base() + address, 0x3000, PAGE_READWRITE, &previous);
            memory->release(address);
        }
    } allocation;
    check(allocation.address != 0, "Dispatcher validation fixture allocation failed");
    const uint32_t boundary = allocation.address + 4096;
    const uint32_t edge = boundary - 16;
    const uint32_t inside = allocation.address + 64;
    auto protectTail = [&](DWORD protection) {
        DWORD previous;
        check(VirtualProtect(base + boundary, 4096, protection, &previous) != 0,
              "Dispatcher tail protection failed");
    };
    auto seed = [&](uint32_t address, uint8_t type, uint32_t state, uint32_t limit = 3) {
        std::memset(base + address, 0, 16);
        base[address] = type;
        memory->write32(address + 4, state);
        if (type == 5) memory->write32(address + 16, limit);
    };
    auto release = [&](uint32_t address) {
        PPCContext ctx{};
        ctx.r3.u64 = address;
        ctx.r5.u64 = 1;
        __imp__KeReleaseSemaphore(ctx, base);
        return ctx.r3.u32;
    };
    auto signal = [&](uint32_t address) {
        PPCContext ctx{};
        ctx.r3.u64 = address;
        __imp__KeSetEvent(ctx, base);
        return ctx.r3.u32;
    };
    // Isolated, synchronous calls: the query counters verify real imports,
    // including boundary fallback, without exposing a second validator.
    struct Profile {
        bool previous = profileEngineCpu;
        Profile() { profileEngineCpu = true; }
        ~Profile() { profileEngineCpu = previous; }
    } profile;
    auto queryCount = [&] {
        return enginePhases[size_t(EnginePhase::queryDispatcher)].calls.load(std::memory_order_relaxed);
    };
    seed(inside, 5, 1);
    uint64_t before = queryCount();
    check(release(inside) == 1 && memory->read32(inside + 4) == 2,
          "Dispatcher rejected common semaphore release");
    check(queryCount() - before == 1, "Dispatcher common semaphore queried its region twice");

    for (DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS),
                             DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
        protectTail(PAGE_READWRITE);
        seed(edge, 5, 1);
        protectTail(protection);
        before = queryCount();
        check(release(edge) == 0xc000000du && memory->read32(edge + 4) == 1,
              "Dispatcher accepted an unwritable semaphore tail or mutated its header");
        check(queryCount() - before == 2, "Dispatcher did not query an uncovered semaphore tail");
        MEMORY_BASIC_INFORMATION unchanged{};
        check(VirtualQuery(base + boundary, &unchanged, sizeof(unchanged)) != 0 &&
              unchanged.Protect == protection,
              "Dispatcher validation consumed a guard or changed tail protection");
        seed(edge, 1, 0);
        before = queryCount();
        check(signal(edge) == 0 && memory->read32(edge + 4) == 1,
              "Dispatcher required a semaphore tail for a complete 16-byte event");
        check(queryCount() - before == 1, "Dispatcher event queried outside its header");
    }
    protectTail(PAGE_READWRITE);
    seed(edge, 5, 1);
    // Different writable regions may span the original 20-byte contract.
    protectTail(PAGE_EXECUTE_READWRITE);
    before = queryCount();
    check(release(edge) == 1 && memory->read32(edge + 4) == 2,
          "Dispatcher rejected a semaphore across writable protection regions");
    check(queryCount() - before == 2, "Dispatcher split semaphore skipped its second writable region");
    // A successful previous call must never supply permissions to this call.
    protectTail(PAGE_READONLY);
    check(release(edge) == 0xc000000du && memory->read32(edge + 4) == 2,
          "Dispatcher retained stale tail permissions between operations");
    protectTail(PAGE_READWRITE);
    check(release(edge) == 2 && memory->read32(edge + 4) == 3,
          "Dispatcher did not observe restored tail permissions");
    seed(inside, 5, 1, 0);
    check(release(inside) == 0xc000000du && memory->read32(inside + 4) == 1,
          "Dispatcher accepted an invalid semaphore limit");
    seed(inside, 5, 4, 3);
    check(release(inside) == 0xc000000du && memory->read32(inside + 4) == 4,
          "Dispatcher accepted a semaphore state above its limit");
    check(release(0) == 0xc000000du && release(inside + 1) == 0xc000000du &&
          release(0xfffffff0u) == 0xc000000du,
          "Dispatcher accepted null, unaligned or end-of-address-space semaphore");
    puts("Dispatcher validation: fresh permissions, exact event/semaphore tails and one-query common path verified.");
}
