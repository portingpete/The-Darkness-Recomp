#include "stall_profiler.h"
#include "developer_tools.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstdlib>

namespace {
using namespace DarkRecomp::Native;

bool readable(uint8_t* base, uint32_t address, size_t bytes) {
    if (!memory || base != memory->base() || !address || !bytes ||
        uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    auto* cursor = base + address;
    const auto* end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = info.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READ && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) return false;
        cursor = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

bool probeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_DEVELOPER_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

void probeUpdate(uint8_t* base, uint32_t application, uint32_t caller,
                 const char* function, unsigned& count) {
    if (!probeEnabled() || count >= 8) return;
    ++count;
    const auto table = readable(base, application, 4) ? PPC_LOAD_U32(application) : 0;
    const bool mapped = readable(base, application, 4909);
    std::fprintf(stderr, "[DeveloperUpdate] function=%s app=%08x table=%08x caller=%08x frontend=%08x preload=%u\n",
        function, application, table, caller,
        mapped ? PPC_LOAD_U32(application + 60) : 0,
        mapped ? unsigned(PPC_LOAD_U8(application + 4908)) : 0);
}

uint32_t currentClient(uint8_t* base, uint32_t application) {
    // CGameContext's TArray of client smart references: 820F68D8 iterates
    // app+3668, reads its count at +4, and its four-byte entries at +24.
    const auto array = PPC_LOAD_U32(application + 3668);
    const bool arrayMapped = readable(base, array, 28);
    const auto count = arrayMapped ? int32_t(PPC_LOAD_U32(array + 4)) : 0;
    const auto entries = arrayMapped ? PPC_LOAD_U32(array + 24) : 0;
    const bool entriesMapped = count > 0 && count <= 16 && readable(base, entries, size_t(count) * 4);
    const auto finish = [&](uint32_t selected) {
        if (probeEnabled()) {
            struct State {
                uint32_t application, appTable, frontend, preload, acceptsMap, array, entries;
                int32_t count;
                uint32_t first, firstTable, selected, selectedTable, playerId, clientFlags, server, serverTable;
                bool arrayMapped, entriesMapped, firstMapped, ready;
                bool operator==(const State&) const = default;
            };
            const auto first = entriesMapped ? PPC_LOAD_U32(entries) : 0;
            const auto server = PPC_LOAD_U32(application + 3660);
            const auto frontend = PPC_LOAD_U32(application + 60);
            const auto preload = unsigned(PPC_LOAD_U8(application + 4908));
            const auto acceptsMap = unsigned(PPC_LOAD_U8(application + 1820));
            const State observed{application, PPC_LOAD_U32(application), frontend, preload, acceptsMap, array, entries, count,
                first, readable(base, first, 4) ? PPC_LOAD_U32(first) : 0,
                selected, selected ? PPC_LOAD_U32(selected) : 0,
                selected ? PPC_LOAD_U32(selected + 536) : 0,
                selected ? PPC_LOAD_U32(selected + 516) : 0,
                server, readable(base, server, 4) ? PPC_LOAD_U32(server) : 0,
                arrayMapped, entriesMapped, readable(base, first, 8980), frontend != 0 && preload == 0};
            struct Previous { State state{}; bool have = false; unsigned lines = 0; };
            thread_local Previous previous;
            if ((!previous.have || !(previous.state == observed)) && previous.lines < 64) {
                ++previous.lines;
                std::fprintf(stderr, "[DeveloperClient] app=%08x table=%08x frontend=%08x preload=%u acceptsMap=%u ready=%u "
                    "array=%08x count=%d data=%08x arrayMapped=%u entriesMapped=%u "
                    "first=%08x firstTable=%08x firstMapped=%u selected=%08x selectedTable=%08x "
                    "localPlayerId=%08x clientFlags=%08x server=%08x serverTable=%08x\n",
                    observed.application, observed.appTable, observed.frontend, observed.preload, observed.acceptsMap, unsigned(observed.ready),
                    observed.array, observed.count, observed.entries, unsigned(observed.arrayMapped), unsigned(observed.entriesMapped),
                    observed.first, observed.firstTable, unsigned(observed.firstMapped), observed.selected, observed.selectedTable,
                    observed.playerId, observed.clientFlags, observed.server, observed.serverTable);
            }
            previous.state = observed; previous.have = true;
        }
        return selected;
    };
    if (!entriesMapped) return finish(0);
    for (int32_t i = 0; i < count; ++i) {
        const auto client = PPC_LOAD_U32(entries + uint32_t(i) * 4);
        if (!readable(base, client, 8980)) continue;
        const auto table = PPC_LOAD_U32(client);
        if (table == 0x820807E0 || table == 0x82081BF0) return finish(client);
    }
    return finish(0);
}
}

namespace DarkRecomp::Native {
// The executable calls this anchor before guest entry. Otherwise a static
// archive can satisfy generated references with its weak PPC fallback without
// pulling this translation unit's strong update wrappers into the game.
void initializeDeveloperTools() noexcept {}
}

extern "C" PPC_FUNC(__imp__sub_820C5D30);
PPC_FUNC(sub_820C5D30) {
    DarkRecomp::Native::StallProfiler::Scope stallProfile(DarkRecomp::Native::StallProfiler::Section::Guest, __func__, 0x820C5D30u, uint32_t(ctx.lr));
    const auto application = ctx.r3.u32;
    thread_local unsigned probes = 0;
    probeUpdate(base, application, uint32_t(ctx.lr), "820C5D30", probes);
    __imp__sub_820C5D30(ctx, base);
    if (!developerToolsNeedsUpdate() && !probeEnabled()) return;
    // Finish the whole original Mod update, including its client/GUI work,
    // before any console transition can invalidate the objects it used.
    if (!readable(base, application, 5024)) return;
    const auto table = PPC_LOAD_U32(application);
    // Original 820C2628 installs 82051EA0. The second retail Mod table has
    // the same 820C5D30 update at +144 and a distinct constructor entry.
    if ((table != 0x82051EA0 && table != 0x82055458) ||
        !readable(base, table + 144, 4) || PPC_LOAD_U32(table + 144) != 0x820C5D30) return;
    // 820F68D8 requires app+60; app+4908 marks a pending transition.
    // The normal frontend has app+1820 clear. Profile completion establishes
    // that context before the map load. Original 820F68D8 dispatches a pending
    // profile callback when app+4112 is nonzero; 820FE3B0 clears it only after
    // completion, so do not replace its staged mission while it is pending.
    const bool ready = PPC_LOAD_U32(application + 60) != 0 &&
                       PPC_LOAD_U8(application + 4908) == 0 &&
                       PPC_LOAD_U8(application + 4112) == 0;
    processDeveloperTools(ctx, base, currentClient(base, application), ready);
}

extern "C" PPC_FUNC(__imp__sub_820F68D8);
PPC_FUNC(sub_820F68D8) {
    DarkRecomp::Native::StallProfiler::Scope stallProfile(DarkRecomp::Native::StallProfiler::Section::Guest, __func__, 0x820F68D8u, uint32_t(ctx.lr));
    const auto application = ctx.r3.u32;
    thread_local unsigned probes = 0;
    probeUpdate(base, application, uint32_t(ctx.lr), "820F68D8", probes);
    __imp__sub_820F68D8(ctx, base);
    if (!developerToolsNeedsUpdate() && !probeEnabled()) return;
    if (!readable(base, application, 4909)) return;
    const auto table = PPC_LOAD_U32(application);
    // The base retail CGameContext uses this general update directly. Mod
    // applications call it inside 820C5D30 and process only after that outer
    // wrapper returns, so those calls must not consume requests here.
    if (table != 0x82060D88 || !readable(base, table + 144, 4) ||
        PPC_LOAD_U32(table + 144) != 0x820F68D8) return;
    const bool ready = PPC_LOAD_U32(application + 60) != 0 &&
                       PPC_LOAD_U8(application + 4908) == 0 &&
                       PPC_LOAD_U8(application + 4112) == 0;
    processDeveloperTools(ctx, base, currentClient(base, application), ready);
}
