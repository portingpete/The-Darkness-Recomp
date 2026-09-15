#pragma once
#include <array>

// Runs in the existing native suite, using real kernel imports rather than
// exposing a second validation implementation just for tests.
static void testCViewBoundaries(PPCContext& ctx) {
    auto* base = memory->base();
    for (uint32_t view = Memory::cAliasBegin; view < Memory::cAliasEnd; view += Memory::cViewBytes) {
        MEMORY_BASIC_INFORMATION info{};
        check(VirtualQuery(base+view, &info, sizeof(info)) != 0 &&
              info.AllocationBase == base+view && info.Type == MEM_MAPPED &&
              static_cast<uint8_t*>(info.BaseAddress)+info.RegionSize <= base+Memory::cViewEnd(view),
              "C mapping is not bounded by its owned 16 MiB view");
    }
    constexpr uint32_t bytes = Memory::cViewBytes+0x4000;
    struct Allocation {
        uint32_t address = 0;
        ~Allocation() { if (address) memory->release(address); }
    } allocation;
    uint32_t boundary = 0;
    for (uint32_t edge = Memory::cAliasBegin+Memory::cViewBytes;
         edge+Memory::cViewBytes < Memory::cAliasEnd; edge += Memory::cViewBytes) {
        const uint32_t a = 0xa0000000u+(edge-Memory::cAliasBegin)-0x2000;
        allocation.address = memory->allocate(bytes, 4096, a, uint64_t(a)+bytes);
        if (allocation.address) { boundary = edge; break; }
    }
    check(allocation.address != 0, "Cannot allocate across three C views");
    const uint32_t a = allocation.address;
    const uint32_t c = Memory::cAliasBegin+Memory::physicalAddress(a);
    const uint32_t e = Memory::cAliasEnd+Memory::physicalAddress(a)-4096;
    const std::array<uint32_t, 6> probes{0, 0x1ffc, 0x2000,
        Memory::cViewBytes+0x1ffc, Memory::cViewBytes+0x2000, bytes-4};
    auto protections = [&](DWORD expected) {
        for (uint32_t alias : {a,c,e}) for (uint32_t offset : probes) {
            MEMORY_BASIC_INFORMATION info{};
            check(VirtualQuery(base+alias+offset, &info, sizeof(info)) != 0 &&
                  info.State == MEM_COMMIT && info.Protect == expected,
                  "Physical protection failed across a C view or A/E alias");
        }
    };
    protections(PAGE_READWRITE);
    for (uint32_t offset : probes) {
        memory->write32(a+offset, 0x11223344u+offset);
        check(memory->read32(c+offset) == 0x11223344u+offset &&
              memory->read32(e+offset) == 0x11223344u+offset, "C view backing offset or E bias changed");
        memory->write32(c+offset, 0x55667788u+offset);
        check(memory->read32(a+offset) == 0x55667788u+offset, "C view write is not shared with A");
        memory->write32(e+offset, 0x99aabbccu+offset);
        check(memory->read32(a+offset) == 0x99aabbccu+offset &&
              memory->read32(c+offset) == 0x99aabbccu+offset, "E write is not shared across C views");
    }
    auto protectPage = [&](uint32_t address, DWORD protection) {
        // Raw VirtualProtect's native contract is one view per call. Keep
        // direct page protection tests; do not route them through a cache.
        DWORD previous;
        check(VirtualProtect(base+address, 4096, protection, &previous) != 0,
              "Cannot set page-local C protection");
    };
    auto seedRequest = [&](uint32_t address) {
        const uint32_t writableAlias = 0xa0000000u+Memory::physicalAddress(address);
        memory->write32(writableAlias, 2);
        memory->write32(writableAlias+4, 0);
        memory->write32(writableAlias+8, 0);
    };
    auto readRequest = [&](uint32_t address) {
        ctx.r3.u64=0xfa; ctx.r4.u64=0x7001a; ctx.r5.u64=0;
        ctx.r6.u64=address; ctx.r7.u64=12;
        __imp__XMsgStartIORequestEx(ctx, base);
        return ctx.r3.u32;
    };
    for (uint32_t edge : {boundary, boundary+Memory::cViewBytes}) {
        seedRequest(edge-4);
        check(readRequest(edge-4) == 0, "Kernel rejected readable artificial C boundary");
        // A zero-context submission exercises the writable group validator
        // and the unchanged original leaf, without launching decode work.
        memory->write32(edge-4, 0);
        memory->write32(edge, 0x30000);
        memory->write32(edge+4, 0);
        ctx.r3.u64=edge-4;
        sub_828B1BA8(ctx, base);
        check(ctx.r3.u32 == 0 && memory->read32(edge) == 0,
              "Kernel rejected writable artificial C boundary or changed original group flags");
    }
    seedRequest(boundary-4);
    for (DWORD protection : {DWORD(PAGE_READONLY), DWORD(PAGE_READWRITE|PAGE_GUARD),
                            DWORD(PAGE_NOACCESS)}) {
        protectPage(boundary, protection);
        check(readRequest(boundary-4) == 0x80070057u,
              "Kernel stitched a genuine protection change at an artificial C boundary");
        ctx.r3.u64=boundary;
        __imp__XMACreateContext(ctx, base);
        check(ctx.r3.u32 == 0xc000000du, "Kernel accepted a denied writable C output");
        MEMORY_BASIC_INFORMATION unchanged{};
        check(VirtualQuery(base+boundary, &unchanged, sizeof(unchanged)) != 0 &&
              unchanged.Protect == protection,
              "Kernel validation consumed a guard or changed input protection");
        check(memory->commit(a, bytes), "Internal cross-view commit could not restore changed protection");
        protections(PAGE_READWRITE);
        check(readRequest(boundary-4) == 0, "Kernel retained stale C protection after commit");
    }
    protectPage(boundary-4096, PAGE_READONLY);
    protectPage(boundary, PAGE_READONLY);
    check(readRequest(boundary-4) == 0, "Matching readonly C views were not stitched");
    check(memory->commit(c, bytes), "C-address commit across three views failed");
    protections(PAGE_READWRITE);
    // An ordinary page boundary inside one view must still reject even when
    // both regions are readable (RW versus RO).
    seedRequest(boundary+4096-4);
    protectPage(boundary+4096, PAGE_READONLY);
    check(readRequest(boundary+4096-4) == 0x80070057u,
          "Kernel broadened single-real-region acceptance inside a C view");
    check(memory->commit(e, bytes), "E-address commit across three C views failed");
    protections(PAGE_READWRITE);
    protectPage(boundary, PAGE_READWRITE|PAGE_GUARD);
    check(memory->release(a), "Release across guarded C views failed");
    allocation.address = 0;
    protections(PAGE_NOACCESS);
    check(readRequest(boundary-4) == 0x80070057u, "Kernel accepted released C input");
    allocation.address = memory->allocate(bytes, 4096, a, uint64_t(a)+bytes);
    check(allocation.address == a, "C view splitting changed exact physical allocation reuse");
    protections(PAGE_READWRITE);
    for (uint32_t offset : probes)
        check(memory->read32(c+offset) == 0, "Reused C backing was not zeroed");
    check(memory->releaseContaining(a+bytes-4), "Interior release across C views failed");
    allocation.address = 0;
    protections(PAGE_NOACCESS);
    puts("Native C views preserve A/C/E backing, cross-view commit/release, fresh permissions and kernel real-region boundaries.");
}
