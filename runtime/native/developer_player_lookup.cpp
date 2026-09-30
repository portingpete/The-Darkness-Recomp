#include "developer_player_lookup.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace DarkRecomp::Native {
namespace {
constexpr uint32_t kSystemGlobal = 0x82A690F8;
constexpr uint32_t kGameContextName = 0x820515EC;
constexpr uint32_t kGameContextTypeToken = 0x82A40308;
constexpr uint32_t kServerModTable = 0x82072370;
constexpr uint32_t kServerP6Table = 0x82080F80;
constexpr uint32_t kServerActorLookup = 0x82448928;
constexpr uint32_t kFrameSize = 192;

bool lookupProbeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DARK_DEVELOPER_PROBE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

struct PlayerLookupProbe {
    uint32_t client = 0, clientTable = 0, clientFlags = 0, playerId = 0;
    uint32_t gameContext = 0, preloading = 0, server = 0, serverTable = 0, actorLookup = 0;
    uint32_t objectCount = 0, actor = 0, actorId = 0, templateId = 0, state = 0;
    bool operator==(const PlayerLookupProbe&) const = default;
};

void reportLookupFailure(const PlayerLookupProbe& probe, const char* stage) {
    if (!probe.client || !lookupProbeEnabled()) return;
    thread_local PlayerLookupProbe previous;
    thread_local const char* previousStage = nullptr;
    thread_local unsigned count = 0;
    if (count >= 48 || (previous == probe && previousStage == stage)) return;
    previous = probe;
    previousStage = stage;
    ++count;
    std::fprintf(stderr, "[DeveloperPlayerLookup] stage=%s client=%08x table=%08x flags=%08x id=%u context=%08x preload=%u server=%08x serverTable=%08x lookup540=%08x count=%u actor=%08x actorId=%u templateId=%u state=%08x\n",
        stage, probe.client, probe.clientTable, probe.clientFlags, probe.playerId,
        probe.gameContext, probe.preloading, probe.server, probe.serverTable, probe.actorLookup,
        probe.objectCount, probe.actor, probe.actorId, probe.templateId, probe.state);
}

bool mapped(uint8_t* base, uint32_t address, size_t bytes, bool writable = false) {
    if (!memory || base != memory->base() || !address || !bytes ||
        uint64_t(address) + bytes > PPC_MEMORY_SIZE) return false;
    auto* cursor = base + address;
    const auto* end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(cursor, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto protection = info.Protect & 0xff;
        const bool canWrite = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool canRead = canWrite || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if (!canRead || (writable && !canWrite)) return false;
        cursor = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}
bool object(uint8_t* base, uint32_t address, size_t bytes, bool writable = false) {
    return !(address & 3) && mapped(base, address, bytes, writable);
}
bool callable(uint8_t* base, uint32_t address) {
    return !(address & 3) && address >= PPC_CODE_BASE &&
        uint64_t(address) < uint64_t(PPC_CODE_BASE) + PPC_CODE_SIZE &&
        PPC_LOOKUP_FUNC(base, address) != nullptr;
}
uint32_t method(uint8_t* base, uint32_t address, uint32_t offset) {
    if (!object(base, address, 4)) return 0;
    const auto table = PPC_LOAD_U32(address);
    if (!object(base, table, size_t(offset) + 4)) return 0;
    const auto function = PPC_LOAD_U32(table + offset);
    return callable(base, function) ? function : 0;
}

class GameContextReference {
public:
    GameContextReference(PPCContext& source, uint8_t* base) : call_(source), base_(base) {}
    void query() {
        auto* base = base_;
        if (!mapped(base_, kSystemGlobal, 4) || !mapped(base_, kGameContextTypeToken, 4) ||
            !mapped(base_, kGameContextName, 12) || call_.r1.u32 < kFrameSize ||
            !mapped(base_, call_.r1.u32 - kFrameSize, kFrameSize, true)) return;
        const auto token = PPC_LOAD_U32(kGameContextTypeToken);
        const auto system = PPC_LOAD_U32(kSystemGlobal);
        const auto query = method(base_, system, 36);
        if (!token || !query) return;
        call_.r1.u64 -= kFrameSize;
        reference_ = call_.r1.u32 + 96;
        PPC_STORE_U32(reference_, 0);
        // Original820D80D0/824A7828: raw service-name pointer and a retained
        // four-byte reference result, rather than a CStr or borrowed pointer.
        call_.r3.u64 = reference_; call_.r4.u64 = system; call_.r5.u64 = kGameContextName;
        PPCSafeIndirect(call_, base_, query);
        const auto candidate = PPC_LOAD_U32(reference_);
        const auto type = method(base_, candidate, 0);
        if (!type || !object(base_, candidate, 4909)) return;
        call_.r3.u64 = candidate;
        PPCSafeIndirect(call_, base_, type);
        auto descriptor = call_.r3.u32;
        for (unsigned depth = 0; descriptor && depth < 64; ++depth) {
            if (!object(base_, descriptor, 12)) return;
            if (PPC_LOAD_U32(descriptor) == token) { gameContext_ = candidate; return; }
            descriptor = PPC_LOAD_U32(descriptor + 8);
        }
    }
    ~GameContextReference() {
        if (!reference_) return;
        auto* base = base_;
        const auto retained = PPC_LOAD_U32(reference_);
        // A malformed service cannot be sent to the original reference
        // destructor. Real retained references still use its atomic release.
        if (retained && !object(base_, retained, 8, true)) return;
        call_.r3.u64 = reference_;
        sub_820C1100(call_, base_);
    }
    uint32_t get() const { return gameContext_; }
    PPCContext& call() { return call_; }
private:
    PPCContext call_;
    uint8_t* base_;
    uint32_t reference_ = 0, gameContext_ = 0;
};
}

DeveloperPlayerHandles resolveDeveloperPlayer(PPCContext& ctx, uint8_t* base, uint32_t client) {
    PlayerLookupProbe probe;
    probe.client = client;
    const auto failed = [&](const char* stage) -> DeveloperPlayerHandles {
        reportLookupFailure(probe, stage);
        return {};
    };
    if (!object(base, client, 540)) return failed("unmapped-client");
    const auto table = PPC_LOAD_U32(client);
    probe.clientTable = table;
    probe.clientFlags = PPC_LOAD_U32(client + 516);
    const auto playerId = PPC_LOAD_U32(client + 536); // Original823ED948. Camera+540 may identify an NPC.
    probe.playerId = playerId;
    if (table != 0x820807E0 && table != 0x82081BF0) return failed("client-type");
    if (probe.clientFlags & 0x20) return failed("client-inactive");
    if (!playerId || playerId > uint32_t((std::numeric_limits<int32_t>::max)())) return failed("player-id");
    GameContextReference context(ctx, base);
    context.query();
    const auto gameContext = context.get();
    probe.gameContext = gameContext;
    if (!gameContext) return failed("game-context");
    probe.preloading = PPC_LOAD_U8(gameContext + 4908);
    if (probe.preloading) return failed("preloading");
    // CGameContext's +3660 single server is distinct from +3668 clients.
    const auto server = PPC_LOAD_U32(gameContext + 3660);
    probe.server = server;
    if (!object(base, server, 1464)) return failed("unmapped-server");
    probe.serverTable = PPC_LOAD_U32(server);
    if (lookupProbeEnabled() && object(base, probe.serverTable, 544))
        probe.actorLookup = PPC_LOAD_U32(probe.serverTable + 540);
    // Retail campaign constructor82378220 installs CWServer_Mod; the P6
    // factory82404368 calls that constructor then installs its derived table.
    // Both retain the original server actor-vector lookup at virtual+540.
    if (probe.serverTable != kServerModTable && probe.serverTable != kServerP6Table)
        return failed("server-type");
    if (method(base, server, 540) != kServerActorLookup) return failed("server-lookup");
    // Validate the exact spans read by the original lookup before calling it.
    // The original function supplies the actor; no host-side array substitute.
    const auto objects = PPC_LOAD_U32(server + 1460);
    if (!object(base, objects, 28)) return failed("server-objects");
    const auto count = PPC_LOAD_U32(objects + 4);
    probe.objectCount = count;
    if (count > uint32_t((std::numeric_limits<int32_t>::max)()) || playerId >= count) return failed("object-count");
    const auto items = PPC_LOAD_U32(objects + 24);
    const uint64_t selected = uint64_t(items) + uint64_t(playerId) * 4;
    if (selected > (std::numeric_limits<uint32_t>::max)() || !object(base, uint32_t(selected), 4)) return failed("object-item");
    auto& call = context.call();
    call.r3.u64 = server; call.r4.u64 = playerId;
    PPCSafeIndirect(call, base, kServerActorLookup);
    const auto actor = call.r3.u32;
    probe.actor = actor;
    if (!object(base, actor, 640)) return failed("unmapped-actor");
    probe.actorId = PPC_LOAD_U16(actor + 368);
    probe.templateId = PPC_LOAD_U16(actor + 370);
    const auto state = PPC_LOAD_U32(actor + 396);
    probe.state = state;
    if (PPC_LOAD_U32(actor + 636) != server || !probe.actorId || !probe.templateId) return failed("actor-identity");
    if (!object(base, state, 10244)) return failed("actor-state");
    return {server, actor, state};
}

bool developerMissionTransitionPending(PPCContext& ctx, uint8_t* base) {
    GameContextReference context(ctx, base);
    context.query();
    return context.get() && PPC_LOAD_U8(context.get() + 4908) != 0;
}
}
