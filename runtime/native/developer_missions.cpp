#include "developer_missions.h"
#include "runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

namespace DarkRecomp::Native {
namespace {
struct Destination { DeveloperMission mission; std::string_view map, mask; };
// Retail Sv.xcr LEVELKEYS/LAYER/SPAWNMASK supplies these chapter variants.
// CubeWnd.xcr Milestone_Gameplay sets DEFAULTSPAWNFLAGS before campaignmap.
// The 27 map bases also have shipped Common, Precache and layer XDF archives.
constexpr Destination destinations[]{
    {{"NY1_Tunnel:ny1+layer1", "Chapter 1: Tunnel (chapter start)"}, "NY1_Tunnel", "ny1+layer1"},
    {{"NY1_Consite:ny1+layer1", "Chapter 1: Construction site"}, "NY1_Consite", "ny1+layer1"},
    {{"NY1_Cemetery:ny1+layer1", "Chapter 1: Cemetery - stage 1"}, "NY1_Cemetery", "ny1+layer1"},
    {{"NY1_Cemetery:ny1+layer2", "Chapter 1: Cemetery - stage 2"}, "NY1_Cemetery", "ny1+layer2"},
    {{"NY1_Chinatown:ny1+layer1", "Chapter 1: Chinatown"}, "NY1_Chinatown", "ny1+layer1"},
    {{"NY1_Pier19:ny1+layer1", "Chapter 1: Pier 19"}, "NY1_Pier19", "ny1+layer1"},
    {{"NY1_Butcher:ny1+layer1", "Chapter 1: Butcher"}, "NY1_Butcher", "ny1+layer1"},
    {{"NY1_Hunter:ny1+layer1", "Chapter 1: Hunter"}, "NY1_Hunter", "ny1+layer1"},
    {{"NY1_Grinder:ny1+layer1", "Chapter 1: Grinder - stage 1"}, "NY1_Grinder", "ny1+layer1"},
    {{"NY1_Grinder:ny1+layer2", "Chapter 1: Grinder - stage 2"}, "NY1_Grinder", "ny1+layer2"},
    {{"NY1_Orphanage:ny1+layer2", "Chapter 1: Orphanage"}, "NY1_Orphanage", "ny1+layer2"},
    {{"SUB_Fulton:ny1+layer1", "Chapter 1: Fulton station - stage 1"}, "SUB_Fulton", "ny1+layer1"},
    {{"SUB_Fulton:ny1+layer2", "Chapter 1: Fulton station - stage 2"}, "SUB_Fulton", "ny1+layer2"},
    {{"SUB_Fulton:ny1+layer3", "Chapter 1: Fulton station - stage 3"}, "SUB_Fulton", "ny1+layer3"},
    {{"SUB_Canal:ny1+layer1", "Chapter 1: Canal station - stage 1"}, "SUB_Canal", "ny1+layer1"},
    {{"SUB_Canal:ny1+layer2", "Chapter 1: Canal station - stage 2"}, "SUB_Canal", "ny1+layer2"},
    {{"NY2_GunHill:ny1+layer1", "Chapter 1: Gun Hill"}, "NY2_GunHill", "ny1+layer1"},
    {{"OW1_Trench:ow1+layer1", "Chapter 2: Trench (chapter start)"}, "OW1_Trench", "ow1+layer1"},
    {{"OW1_Village:ow1+layer1", "Chapter 2: Village"}, "OW1_Village", "ow1+layer1"},
    {{"OW1_Sewers:ow1+layer1", "Chapter 2: Sewers"}, "OW1_Sewers", "ow1+layer1"},
    {{"OW1_Hills:ow1+layer1", "Chapter 2: Hills"}, "OW1_Hills", "ow1+layer1"},
    {{"OW1_Cannon:ow1+layer1", "Chapter 2: Cannon"}, "OW1_Cannon", "ow1+layer1"},
    {{"NY1_Cemetery:ny2+layer1", "Chapter 3: Cemetery - stage 1"}, "NY1_Cemetery", "ny2+layer1"},
    {{"NY1_Cemetery:ny2+layer2", "Chapter 3: Cemetery - stage 2"}, "NY1_Cemetery", "ny2+layer2"},
    {{"NY1_Chinatown:ny2+layer1", "Chapter 3: Chinatown"}, "NY1_Chinatown", "ny2+layer1"},
    {{"NY1_Pier19:ny2+layer1", "Chapter 3: Pier 19"}, "NY1_Pier19", "ny2+layer1"},
    {{"NY1_Grinder:ny2+layer1", "Chapter 3: Grinder"}, "NY1_Grinder", "ny2+layer1"},
    {{"NY1_Orphanage:ny2+layer1", "Chapter 3: Orphanage"}, "NY1_Orphanage", "ny2+layer1"},
    {{"SUB_Fulton:ny2+layer1", "Chapter 3: Fulton station - stage 1"}, "SUB_Fulton", "ny2+layer1"},
    {{"SUB_Fulton:ny2+layer2", "Chapter 3: Fulton station - stage 2"}, "SUB_Fulton", "ny2+layer2"},
    {{"SUB_Fulton:ny2+layer3", "Chapter 3: Fulton station - stage 3"}, "SUB_Fulton", "ny2+layer3"},
    {{"SUB_Fulton:ny2+layer4", "Chapter 3: Fulton station - stage 4"}, "SUB_Fulton", "ny2+layer4"},
    {{"SUB_Canal:ny2+layer1", "Chapter 3: Canal station - stage 1 (chapter start)"}, "SUB_Canal", "ny2+layer1"},
    {{"SUB_Canal:ny2+layer2", "Chapter 3: Canal station - stage 2"}, "SUB_Canal", "ny2+layer2"},
    {{"SUB_Canal:ny2+layer3", "Chapter 3: Canal station - stage 3"}, "SUB_Canal", "ny2+layer3"},
    {{"NY2_Sarah:ny2+layer1", "Chapter 3: Sarah"}, "NY2_Sarah", "ny2+layer1"},
    {{"NY2_GunHill:ny2+layer1", "Chapter 3: Gun Hill - stage 1"}, "NY2_GunHill", "ny2+layer1"},
    {{"NY2_GunHill:ny2+layer2", "Chapter 3: Gun Hill - stage 2"}, "NY2_GunHill", "ny2+layer2"},
    {{"NY2_CityHall:ny2+layer1", "Chapter 3: City Hall - stage 1"}, "NY2_CityHall", "ny2+layer1"},
    {{"NY2_CityHall:ny2+layer2", "Chapter 3: City Hall - stage 2"}, "NY2_CityHall", "ny2+layer2"},
    {{"NY2_Turkish:ny2+layer1", "Chapter 3: Turkish baths"}, "NY2_Turkish", "ny2+layer1"},
    {{"NY2_Church:ny2+layer1", "Chapter 3: Church"}, "NY2_Church", "ny2+layer1"},
    {{"OW1_Village:ow2+layer1", "Chapter 4: Village"}, "OW1_Village", "ow2+layer1"},
    {{"OW2_Castle:ow2+layer1", "Chapter 4: Castle - stage 1 (chapter start)"}, "OW2_Castle", "ow2+layer1"},
    {{"OW2_Castle:ow2+layer2", "Chapter 4: Castle - stage 2"}, "OW2_Castle", "ow2+layer2"},
    {{"OW2_Tankride:ow2+layer1", "Chapter 4: Tank ride"}, "OW2_Tankride", "ow2+layer1"},
    {{"OW2_Darkness:ow2+layer1", "Chapter 4: Darkness"}, "OW2_Darkness", "ow2+layer1"},
    {{"NY1_Cemetery:ny3+layer1", "Chapter 5: Cemetery"}, "NY1_Cemetery", "ny3+layer1"},
    {{"NY1_Chinatown:ny3+layer1", "Chapter 5: Chinatown"}, "NY1_Chinatown", "ny3+layer1"},
    {{"NY1_Pier19:ny3+layer1", "Chapter 5: Pier 19 - stage 1"}, "NY1_Pier19", "ny3+layer1"},
    {{"NY1_Pier19:ny3+layer2", "Chapter 5: Pier 19 - stage 2"}, "NY1_Pier19", "ny3+layer2"},
    {{"NY1_Grinder:ny3+layer1", "Chapter 5: Grinder - stage 1"}, "NY1_Grinder", "ny3+layer1"},
    {{"NY1_Grinder:ny3+layer2", "Chapter 5: Grinder - stage 2"}, "NY1_Grinder", "ny3+layer2"},
    {{"NY1_Orphanage:ny3+layer1", "Chapter 5: Orphanage (chapter start)"}, "NY1_Orphanage", "ny3+layer1"},
    {{"SUB_Fulton:ny3+layer1", "Chapter 5: Fulton station"}, "SUB_Fulton", "ny3+layer1"},
    {{"SUB_Canal:ny3+layer1", "Chapter 5: Canal station"}, "SUB_Canal", "ny3+layer1"},
    {{"NY2_Sarah:ny3+layer1", "Chapter 5: Sarah - stage 1"}, "NY2_Sarah", "ny3+layer1"},
    {{"NY2_Sarah:ny3+layer2", "Chapter 5: Sarah - stage 2"}, "NY2_Sarah", "ny3+layer2"},
    {{"NY2_Sarah:ny3+layer3", "Chapter 5: Sarah - stage 3"}, "NY2_Sarah", "ny3+layer3"},
    {{"NY2_GunHill:ny3+layer1", "Chapter 5: Gun Hill"}, "NY2_GunHill", "ny3+layer1"},
    {{"NY2_CityHall:ny3+layer1", "Chapter 5: City Hall"}, "NY2_CityHall", "ny3+layer1"},
    {{"NY2_Turkish:ny3+layer1", "Chapter 5: Turkish baths"}, "NY2_Turkish", "ny3+layer1"},
    {{"NY3_Ship:ny3+layer1", "Chapter 5: Ship"}, "NY3_Ship", "ny3+layer1"},
    {{"NY3_Mansion:ny3+layer1", "Chapter 5: Mansion"}, "NY3_Mansion", "ny3+layer1"},
    {{"NY3_End:ny3+layer1", "Chapter 5: Ending"}, "NY3_End", "ny3+layer1"},
};

constexpr auto catalog = [] {
    std::array<DeveloperMission, std::size(destinations)> result{};
    for (size_t i = 0; i < result.size(); ++i) result[i] = destinations[i].mission;
    return result;
}();
const Destination* destinationFor(std::string_view id) {
    for (const auto& destination : destinations) if (destination.mission.id == id) return &destination;
    return nullptr;
}

// Native UI never dereferences guest objects. These checks run only at the
// original application update boundary, against the runtime memory mapping.
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
        if (writable && protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        cursor = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}
bool callable(uint8_t* base, uint32_t address) {
    return address >= PPC_CODE_BASE && address < PPC_CODE_BASE + PPC_CODE_SIZE &&
           PPC_LOOKUP_FUNC(base, address) != nullptr;
}
uint32_t method(uint8_t* base, uint32_t object, uint32_t offset) {
    if (!mapped(base, object, 4)) return 0;
    const auto table = PPC_LOAD_U32(object);
    if (!mapped(base, table, size_t(offset) + 4)) return 0;
    const auto address = PPC_LOAD_U32(table + offset);
    return callable(base, address) ? address : 0;
}

constexpr uint32_t systemGlobal = 0x82A690F8;
constexpr uint32_t consoleName = 0x82060ECC; // SYSTEM.CONSOLE
constexpr uint32_t consoleClassName = 0x82096B8C; // CConsole
constexpr uint32_t executeString = 0x82782E28;
// Reproduce the original CubeButton execution bridge 8239DD00, including its
// SYSTEM.CONSOLE query and original runtime class descriptor chain. Retain the
// returned reference until execution is complete; never cache a guest pointer.
uint32_t findConsole(PPCContext& ctx, uint8_t* base, uint32_t reference) {
    const auto system = PPC_LOAD_U32(systemGlobal);
    const auto lookup = method(base, system, 36);
    if (!lookup) return 0;
    PPC_STORE_U32(reference, 0);
    ctx.r3.u64 = reference; ctx.r4.u64 = system; ctx.r5.u64 = consoleName;
    PPCSafeIndirect(ctx, base, lookup);
    const auto object = PPC_LOAD_U32(reference);
    const auto type = method(base, object, 0);
    if (!type || !mapped(base, object, 148)) return 0;
    ctx.r3.u64 = object;
    PPCSafeIndirect(ctx, base, type);
    auto descriptor = ctx.r3.u32;
    for (unsigned depth = 0; descriptor && depth < 64; ++depth) {
        if (!mapped(base, descriptor, 12)) return 0;
        if (PPC_LOAD_U32(descriptor) == consoleClassName) return object;
        descriptor = PPC_LOAD_U32(descriptor + 8);
    }
    return 0;
}
}

std::span<const DeveloperMission> developerMissions() noexcept { return catalog; }

std::string developerMissionCommand(std::string_view id, bool initializeSession) {
    const auto* destination = destinationFor(id);
    if (!destination) return {};
    // A frontend has no campaign session yet: campaignmap's 820FFF28 callback
    // drops its request while app+1820 is zero. The authored Menu_ToFork path
    // uses cg_loadlastvalidprofile (8237E4D8 -> 820FDE38) to stage success/failure
    // scripts. The next original update runs 820FE3B0: signed-in profiles use
    // 820FDBC0; the unsigned branch 820FE644 configures the default profile and
    // writes app+1820=1 before executing the failure callback at 820FE70C.
    // Both completed session paths submit the selected mission. The ordinary
    // campaignmap guard still rejects an actual failed session initialization.
    // This preserves normal profile/checkpoint behavior without New Game's
    // deleteallsavegames reset or dependence on a CubeMenu animation timer.
    const auto spawn = "setgamekey('DEFAULTSPAWNFLAGS','" + std::string(destination->mask) + "'); ";
    const auto load = "cg_blackloading(); campaignmap('" + std::string(destination->map) + "')";
    const auto transition = spawn + load;
    const auto session = "cg_loadlastvalidprofile(\"" + transition + "\", \"" + transition + "\")";
    if (initializeSession) return session;
    // Retail mainmenu_back_fork queues disconnect before deferredscript.
    // Disconnect's message0 calls 820F4C18(app,1), which synchronously clears
    // the current clients/server before the following message6 executes.
    // Start the chosen mission through the same profile/session callbacks as
    // a frontend load, preserving saves and avoiding the previous world's
    // transient player/camera state. Escape the inner profile arguments using
    // the nested string grammar already used by retail cachecommand scripts.
    std::string deferred;
    deferred.reserve(session.size() + 4);
    for (const char c : session) {
        if (c == '"' || c == '\\') deferred += '\\';
        deferred += c;
    }
    return "disconnect(); deferredscript(\"" + deferred + "\")";
}

bool canLoadDeveloperMission(uint8_t* base, uint32_t client) {
    if (!mapped(base, systemGlobal, 4)) return false;
    // A ready frontend has an original console but may have no world client.
    // The application update wrapper separately validates frontend/load state.
    if (client) {
        if (!mapped(base, client, 8980)) return false;
        const auto table = PPC_LOAD_U32(client);
        if (table != 0x820807E0 && table != 0x82081BF0) return false;
        // 0x20 rejects ordinary client command submissions during a transition.
        // 0x40 is an active GUI and is allowed, including the main/pause menus.
        if (PPC_LOAD_U32(client + 516) & 0x20) return false;
    }
    return method(base, PPC_LOAD_U32(systemGlobal), 36) && callable(base, executeString);
}

bool loadDeveloperMission(PPCContext& ctx, uint8_t* base, uint32_t client,
                          std::string_view id, std::string& status) {
    const auto* destination = destinationFor(id);
    if (!destination) { status = "Unknown mission."; return false; }
    if (!canLoadDeveloperMission(base, client)) { status = "Mission loading is unavailable."; return false; }
    const auto directory = memory->gameDirectory() / "Content" / "Xdf";
    std::error_code error;
    for (const auto suffix : {"_Common.XDF", "_Precache.XDF"}) {
        if (!std::filesystem::is_regular_file(directory / (std::string(destination->map) + suffix), error)) {
            status = "The selected mission archives are missing."; return false;
        }
    }
    const auto script = developerMissionCommand(id, client == 0);
    auto call = ctx;
    constexpr uint32_t frameSize = 768;
    if (call.r1.u64 < frameSize || !mapped(base, call.r1.u32 - frameSize, frameSize, true)) {
        status = "The engine command stack is unavailable."; return false;
    }
    call.r1.u64 -= frameSize;
    const auto raw = call.r1.u32 + 128, string = call.r1.u32 + 512,
               ownedString = call.r1.u32 + 528, reference = call.r1.u32 + 544;
    PPC_STORE_U32(reference, 0);
    const auto console = findConsole(call, base, reference);
    struct ReleaseReference {
        PPCContext& call; uint8_t* base; uint32_t reference;
        ~ReleaseReference() { call.r3.u64 = reference; sub_820C1100(call, base); }
    } release{call, base, reference};
    if (!console) { status = "The original campaign console is unavailable."; return false; }
    std::memcpy(base + raw, script.c_str(), script.size() + 1);
    call.r3.u64 = string; call.r4.u64 = raw;
    sub_821F86A8(call, base); // Original CStr owns its allocated backing.
    struct ReleaseString {
        PPCContext& call; uint8_t* base; uint32_t string;
        ~ReleaseString() { call.r3.u64 = string; sub_821F8AD0(call, base); }
    } releaseString{call, base, string};
    if (!PPC_LOAD_U32(string + 4)) { status = "The engine command string could not be allocated."; return false; }
    // ExecuteString consumes its by-value CStr. This is the same original copy
    // constructor/ownership transfer used by 8239DD00, rather than raw bytes.
    call.r3.u64 = ownedString; call.r4.u64 = string;
    sub_821F8918(call, base);
    call.r3.u64 = console; call.r4.u64 = ownedString;
    call.r5.u64 = 0x820555A4; // Original CConsole::ExecuteString error context.
    call.r6.u64 = 0;
    PPCSafeIndirect(call, base, executeString);
    status = "Queued " + std::string(destination->mission.title) +
             " for the next engine update. Loading a mission may autosave.";
    return true;
}
}
