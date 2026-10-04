#pragma once
#include "runtime/native/dispatcher_image_span.h"
#include "renderer/engine/engine_performance.h"
#include <array>

static void testDispatcherImageValidation() {
    auto* base = memory->base();
    constexpr uint32_t imageEnd = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
    constexpr uint32_t begin = imageEnd - 3 * 4096;
    // This branch runs before the dispatcher's worker fixtures. Save the last
    // three image pages and first host-table page, and restore them before any
    // original guest callback can use the table. No mapping layout is changed.
    struct SavedPages {
        uint8_t* pointer;
        std::array<uint8_t, 4 * 4096> bytes{};
        std::array<DWORD, 4> protection{};
        explicit SavedPages(uint8_t* value) : pointer(value) {
            for (unsigned i = 0; i < protection.size(); ++i) {
                MEMORY_BASIC_INFORMATION info{};
                check(VirtualQuery(pointer + i * 4096, &info, sizeof(info)) &&
                      info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
                      "Dispatcher image fixture is not ordinary committed image/table memory");
                protection[i] = info.Protect;
            }
            std::memcpy(bytes.data(), pointer, bytes.size());
        }
        ~SavedPages() {
            for (unsigned i = 0; i < protection.size(); ++i) {
                auto* page = pointer + i * 4096;
                DWORD old{};
                if (!VirtualAlloc(page, 4096, MEM_COMMIT, PAGE_READWRITE) ||
                    !VirtualProtect(page, 4096, PAGE_READWRITE, &old)) std::terminate();
                std::memcpy(page, bytes.data() + i * 4096, 4096);
                if (!VirtualProtect(page, 4096, protection[i], &old)) std::terminate();
            }
        }
    } saved(base + begin);
    struct Profile {
        bool old = profileEngineCpu;
        Profile() { profileEngineCpu = true; }
        ~Profile() { profileEngineCpu = old; }
    } profile;
    auto queries = [] {
        return std::array<uint64_t, 2>{
            enginePhases[size_t(EnginePhase::queryDispatcherWorkingSet)].calls.load(std::memory_order_relaxed),
            enginePhases[size_t(EnginePhase::queryDispatcher)].calls.load(std::memory_order_relaxed)};
    };
    auto protect = [&](uint32_t page, DWORD value) {
        DWORD old{};
        check(VirtualProtect(base + page, 4096, value, &old), "Dispatcher image page protection failed");
    };
    auto seed = [&](uint32_t address, uint8_t type, uint32_t state = 1) {
        std::memset(base + address, 0, type == 5 ? 20 : 16);
        base[address] = type; memory->write32(address + 4, state);
        if (type == 5) memory->write32(address + 16, 3);
    };
    auto release = [&](uint32_t address) {
        PPCContext call{}; call.r3.u64 = address; call.r5.u64 = 1;
        __imp__KeReleaseSemaphore(call, base); return call.r3.u32;
    };
    auto signal = [&](uint32_t address) {
        PPCContext call{}; call.r3.u64 = address;
        __imp__KeSetEvent(call, base); return call.r3.u32;
    };
    const uint32_t inside = begin + 64, boundary = begin + 4096, edge = boundary - 16;
    const auto initialAccepted = dispatcherWorkingSetAccepted.load(std::memory_order_relaxed);
    seed(inside, 5);
    auto before = queries();
    const auto acceptedBefore = dispatcherWorkingSetAccepted.load(std::memory_order_relaxed);
    check(release(inside) == 1 && memory->read32(inside + 4) == 2,
          "Resident image semaphore changed release behavior");
    auto after = queries();
    check(after[0] - before[0] == 1, "Image semaphore did not query fresh working-set attributes");
    if (dispatcherWorkingSetAccepted.load(std::memory_order_relaxed) != acceptedBefore)
        check(after[1] == before[1], "Resident image fast path also performed VirtualQuery");

    // Each protection change follows a prior successful query and makes no
    // content access until after the import. A guard must remain armed.
    for (DWORD value : {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD),
                        DWORD(PAGE_EXECUTE), DWORD(PAGE_EXECUTE_READ), DWORD(PAGE_EXECUTE_READWRITE)}) {
        protect(begin, PAGE_READWRITE); seed(inside, 5); protect(begin, value);
        before = queries();
        const auto result = release(inside);
        const bool writable = value == PAGE_EXECUTE_READWRITE;
        check(result == (writable ? 1u : 0xc000000du), "Image import ignored a fresh protection change");
        MEMORY_BASIC_INFORMATION untouched{};
        check(VirtualQuery(base + begin, &untouched, sizeof(untouched)) && untouched.Protect == value,
              "Image permission query consumed a guard or changed page protection");
        check(queries()[0] - before[0] == 1, "Image protection transition skipped the fresh query");
        protect(begin, PAGE_READWRITE);
        check(memory->read32(inside + 4) == (writable ? 2u : 1u), "Denied image import changed semaphore state");
    }
    seed(edge, 5);
    protect(boundary, PAGE_EXECUTE_READWRITE);
    before = queries();
    check(release(edge) == 1 && memory->read32(edge + 4) == 2,
          "Image semaphore tail across writable page protections was rejected");
    check(queries()[0] - before[0] == 2, "Image semaphore tail reused permissions beyond the checked page end");
    protect(boundary, PAGE_READWRITE);
    for (DWORD value : {DWORD(PAGE_READONLY), DWORD(PAGE_NOACCESS), DWORD(PAGE_READWRITE | PAGE_GUARD)}) {
        seed(edge, 5); protect(boundary, value);
        check(release(edge) == 0xc000000du && memory->read32(edge + 4) == 1,
              "Image semaphore accepted an inaccessible next-page tail");
        seed(edge, 1, 0);
        before = queries();
        check(signal(edge) == 0 && memory->read32(edge + 4) == 1,
              "Complete image event incorrectly required its next-page tail");
        check(queries()[0] - before[0] == 1, "Image event queried outside its 16-byte header");
        MEMORY_BASIC_INFORMATION untouched{};
        check(VirtualQuery(base + boundary, &untouched, sizeof(untouched)) && untouched.Protect == value,
              "Image tail validation changed a protected page");
        protect(boundary, PAGE_READWRITE);
    }
    seed(boundary - 8, 5);
    before = queries();
    check(release(boundary - 8) == 1 && memory->read32(boundary - 4) == 2,
          "Two-page image header changed semaphore behavior");
    check(queries()[0] - before[0] == 1, "Two-page header failed to prove its same-page semaphore tail");

    seed(inside, 0, 1);
    const uint32_t timeout = boundary + 64;
    for (DWORD value : {DWORD(PAGE_READONLY), DWORD(PAGE_EXECUTE_READ), DWORD(PAGE_EXECUTE)}) {
        protect(boundary, PAGE_READWRITE);
        std::memset(base + timeout, 0, 8);
        protect(boundary, value);
        PPCContext wait{}; wait.r3.u64 = inside; wait.r7.u64 = timeout;
        __imp__KeWaitForSingleObject(wait, base);
        check(wait.r3.u32 == (value == PAGE_EXECUTE ? 0xc000000du : 0u) &&
              memory->read32(inside + 4) == 1,
              "Image timeout read policy confused execute-only with readable protection");
    }
    protect(boundary, PAGE_READWRITE);

    // Cold committed pages have no resident permission proof. The original
    // query still accepts a zeroed manual event and rejects a decommitted one.
    const uint32_t cold = begin + 2 * 4096;
    check(VirtualFree(base + cold, 4096, MEM_DECOMMIT), "Cannot decommit image fixture page");
    before = queries();
    check(signal(cold) == 0xc000000du && queries()[1] - before[1] == 1,
          "Nonresident decommitted image did not use the original rejection path");
    check(VirtualAlloc(base + cold, 4096, MEM_COMMIT, PAGE_READWRITE), "Cannot recommit cold image page");
    const auto nonresidentBefore = dispatcherWorkingSetNonresidentFallback.load(std::memory_order_relaxed);
    before = queries();
    check(signal(cold) == 0 && memory->read32(cold + 4) == 1,
          "Cold committed image event changed acceptance behavior");
    check(dispatcherWorkingSetNonresidentFallback.load(std::memory_order_relaxed) == nonresidentBefore + 1 &&
          queries()[1] - before[1] == 1, "Cold committed image did not fall back to fresh VirtualQuery");

    const uint32_t upperEdge = imageEnd - 16;
    seed(upperEdge, 1, 0); before = queries();
    check(signal(upperEdge) == 0 && queries()[0] - before[0] == 1,
          "Final complete image event was not covered by its own page proof");
    seed(upperEdge, 5); before = queries();
    check(release(upperEdge) == 1 && queries()[0] - before[0] == 1 && queries()[1] - before[1] == 1,
          "Image-edge semaphore skipped its fresh non-image tail query");
    PPCContext init{}; init.r3.u64 = upperEdge; init.r4.u64 = 1; init.r5.u64 = 3;
    before = queries(); __imp__KeInitializeSemaphore(init, base);
    check(init.r3.u32 == 0 && queries()[0] == before[0] && queries()[1] - before[1] == 1,
          "Partially non-image initialization used the bounded image fast path");
    before = queries();
    check(signal(uint32_t(PPC_IMAGE_BASE) - 8) == 0xc000000du && queries()[0] == before[0],
          "Partially below-image event used the bounded image fast path");

    // API failure and guest/image-end arithmetic are independently checked at
    // the helper boundary, without adding test hooks to the real imports.
    const auto failed = dispatcherImageSpan(base, inside, 16, true, [](auto*, DWORD) { return FALSE; });
    check(failed.status == DispatcherImageSpanStatus::fallback && failed.apiFailure && !failed.checkedEnd,
          "Failed working-set API supplied an image permission proof");
    unsigned unexpectedCalls = 0;
    auto unexpected = [&](auto*, DWORD) { ++unexpectedCalls; return FALSE; };
    dispatcherImageSpan(base, 0xfffffff0u, 32, true, unexpected);
    dispatcherImageSpan(base, imageEnd - 8, 16, true, unexpected);
    dispatcherImageSpan(base, begin, 3 * 4096, true, unexpected);
    check(unexpectedCalls == 0, "Image working-set helper queried overflow, outside-image or wide spans");
    std::printf("Dispatcher image validation: resident spans=%llu; fresh permissions, guards, cold fallback and image/page boundaries verified.\n",
        dispatcherWorkingSetAccepted.load(std::memory_order_relaxed) - initialAccepted);
}
