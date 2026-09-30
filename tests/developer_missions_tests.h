#pragma once
#include "runtime/native/developer_missions.h"
#include <set>

namespace DeveloperMissionFixture {
constexpr uint32_t queryAddress = 0x820C1004, typeAddress = 0x820C1014;
constexpr uint32_t executeAddress = 0x82782E28, assignAddress = 0x821F7FC0;
inline uint32_t systemObject, console, descriptor, backing;
inline unsigned queries, executions, assignments;
inline std::string command;
inline bool returnConsole = true;

// Only service lookup and script execution are substituted. The production
// bridge still uses the original CStr constructor, copy/refcount/destructor.
// Its raw allocation callback is isolated from the game's unstarted heap.
static PPC_FUNC(query) {
    check(ctx.r4.u32 == systemObject && ctx.r5.u32 == 0x82060ECC &&
          std::strcmp(reinterpret_cast<char*>(base + ctx.r5.u32), "SYSTEM.CONSOLE") == 0,
          "mission bridge queried the wrong original service");
    ++queries;
    if (returnConsole) {
        memory->write32(console + 4, memory->read32(console + 4) + 1);
        memory->write32(ctx.r3.u32, console);
    } else memory->write32(ctx.r3.u32, 0);
}
static PPC_FUNC(type) {
    check(ctx.r3.u32 == console, "mission bridge inspected the wrong console class");
    ctx.r3.u64 = descriptor;
}
static PPC_FUNC(assign) {
    check(ctx.r5.u32 > 0 && ctx.r5.u32 < 320 && memory->read32(ctx.r3.u32) == 0x82065568,
          "original CStr constructor did not reach its byte assignment callback");
    ++assignments;
    // One fixture reference plus the newly constructed original CStr owner.
    PPC_STORE_U16(backing, 0x4002);
    std::memcpy(base + backing + 2, base + ctx.r4.u32, ctx.r5.u32 + 1);
    memory->write32(ctx.r3.u32 + 4, backing);
}
static PPC_FUNC(execute) {
    check(ctx.r3.u32 == console && ctx.r5.u32 == 0x820555A4 && ctx.r6.u32 == 0,
          "mission bridge changed original ExecuteString arguments");
    const auto string = ctx.r4.u32;
    check(memory->read32(string) == 0x82065568 && memory->read32(string + 4) == backing &&
          PPC_LOAD_U16(backing) == 0x4003,
          "ExecuteString did not receive the original owned by-value CStr copy");
    command = reinterpret_cast<const char*>(base + backing + 2);
    ++executions;
    // ExecuteString consumes its by-value argument (82782E28 -> 821F8AD0).
    ctx.r3.u64 = string;
    sub_821F8AD0(ctx, base);
    ctx.r3.u64 = 0xBAD0BAD0;
    ctx.r31.u64 = 0xFEEDFACE;
}
}

static void testDeveloperMissions(PPCContext& ctx) {
    using namespace DeveloperMissionFixture;
    const auto missions = developerMissions();
    check(missions.size() == 65, "retail mission variants were omitted or invented");
    std::set<std::string_view> ids, maps;
    unsigned chapter = 1;
    for (const auto& mission : missions) {
        check(ids.insert(mission.id).second, "mission IDs are not unique");
        const auto split = mission.id.find(':');
        check(split != std::string_view::npos && split > 0, "mission ID lacks its authored spawn mask");
        maps.insert(mission.id.substr(0, split));
        check(!developerMissionCommand(mission.id).empty(), "catalog mission cannot form a command");
        check(mission.title.starts_with("Chapter ") && mission.title[8] >= '1' && mission.title[8] <= '5',
              "mission label lacks its retail chapter group");
        const unsigned next = unsigned(mission.title[8] - '0');
        check(next >= chapter, "mission catalog is not ordered by chapter");
        chapter = next;
    }
    // Intersection of retail Sv.xcr LEVELKEYS and shipped *_Precache.XDF.
    const std::set<std::string_view> expectedMaps{
        "NY1_Tunnel", "NY1_Consite", "NY1_Cemetery", "NY1_Chinatown", "NY1_Pier19", "NY1_Butcher",
        "NY1_Hunter", "NY1_Grinder", "NY1_Orphanage", "SUB_Fulton", "SUB_Canal", "OW1_Trench",
        "OW1_Village", "OW1_Sewers", "OW1_Hills", "OW1_Cannon", "NY2_Sarah", "NY2_GunHill",
        "NY2_CityHall", "NY2_Turkish", "NY2_Church", "OW2_Castle", "OW2_Tankride", "OW2_Darkness",
        "NY3_Ship", "NY3_Mansion", "NY3_End"};
    check(maps == expectedMaps, "mission catalog differs from shipped campaign map bases");
    for (const auto id : {"", "NY1_Lowereast:ny1+layer1", "demo_tunnel:ny1+layer1",
                           "NY1_Tunnel:ny1+layer2", "NY1_Tunnel:ny1+layer1');deleteallsavegames();", "../NY1_Tunnel"})
        check(developerMissionCommand(id).empty() && developerMissionCommand(id, true).empty(),
              "unvalidated mission reached the command language");
    // Retail mainmenu_back_fork queues disconnect before a deferred script.
    // Restart the session only after its original message0 teardown, using
    // the same signed-in/unsigned profile callbacks proven by frontend loads.
    const std::string tunnel = "disconnect(); deferredscript(\"cg_loadlastvalidprofile(\\\"setgamekey('DEFAULTSPAWNFLAGS','ny1+layer1'); cg_blackloading(); campaignmap('NY1_Tunnel')\\\", \\\"setgamekey('DEFAULTSPAWNFLAGS','ny1+layer1'); cg_blackloading(); campaignmap('NY1_Tunnel')\\\")\")";
    const std::string canal = "disconnect(); deferredscript(\"cg_loadlastvalidprofile(\\\"setgamekey('DEFAULTSPAWNFLAGS','ny2+layer1'); cg_blackloading(); campaignmap('SUB_Canal')\\\", \\\"setgamekey('DEFAULTSPAWNFLAGS','ny2+layer1'); cg_blackloading(); campaignmap('SUB_Canal')\\\")\")";
    const std::string frontendTunnel = "cg_loadlastvalidprofile(\"setgamekey('DEFAULTSPAWNFLAGS','ny1+layer1'); cg_blackloading(); campaignmap('NY1_Tunnel')\", \"setgamekey('DEFAULTSPAWNFLAGS','ny1+layer1'); cg_blackloading(); campaignmap('NY1_Tunnel')\")";
    check(developerMissionCommand("NY1_Tunnel:ny1+layer1") == tunnel &&
          developerMissionCommand("SUB_Canal:ny2+layer1") == canal,
          "running-session mission bypassed original disconnect/deferred session initialization");
    check(developerMissionCommand("NY1_Tunnel:ny1+layer1", true) == frontendTunnel,
          "frontend mission omitted a completed signed-in or unsigned profile/session callback");
    // 820FE644 enables the unsigned session before selecting the second
    // callback, so both original callback bodies must retain the chosen map.
    // Each callback is copied into an original 252-byte fixed string.
    for (const auto& mission : missions) {
        const auto script = developerMissionCommand(mission.id, true);
        const auto first = script.find('"'), separator = script.find("\", \"", first + 1);
        const auto last = script.rfind('"');
        check(first != std::string::npos && separator != std::string::npos && last > separator + 4,
              "profile/session command changed the authored two-script argument grammar");
        const auto success = script.substr(first + 1, separator - first - 1);
        const auto unsignedSession = script.substr(separator + 4, last - separator - 4);
        check(success == unsignedSession && success.size() < 252 && script.size() < 384,
              "unsigned session lost its mission callback or exceeded an original string boundary");
        check(script.find("continuewithoutsave") == std::string::npos &&
              script.find("cant_save_progress") == std::string::npos,
              "mission transition changed native save policy or opened the unsigned-profile prompt");
    }
    for (const auto& mission : missions) {
        const auto split = mission.id.find(':');
        const auto callback = "setgamekey('DEFAULTSPAWNFLAGS','" + std::string(mission.id.substr(split + 1)) +
            "'); cg_blackloading(); campaignmap('" + std::string(mission.id.substr(0, split)) + "')";
        const auto expected = "disconnect(); deferredscript(\"cg_loadlastvalidprofile(\\\"" + callback +
            "\\\", \\\"" + callback + "\\\")\")";
        const auto script = developerMissionCommand(mission.id);
        check(script == expected && script.size() < 384,
              "running-session mission changed disconnect/profile transition order, escaping or command bounds");
        check(script.find("deleteallsavegames") == std::string::npos && script.find("setdifficulty") == std::string::npos &&
              script.find("continuewithoutsave") == std::string::npos,
              "running-session mission reset campaign state or changed native save policy");
    }
    check(frontendTunnel.find("cachecommand") == std::string::npos &&
          frontendTunnel.find("begin_loadtransform") == std::string::npos &&
          frontendTunnel.find("deleteallsavegames") == std::string::npos &&
          frontendTunnel.find("setdifficulty") == std::string::npos,
          "frontend mission still requires a cube-menu timer or destructive new-game reset");
    check(developerMissionCommand("NY1_Grinder:ny1+layer2").find("'ny1+layer2'") != std::string::npos &&
          developerMissionCommand("SUB_Fulton:ny2+layer4").find("'ny2+layer4'") != std::string::npos &&
          developerMissionCommand("OW2_Castle:ow2+layer2").find("'ow2+layer2'") != std::string::npos,
          "return visit selected the wrong authored campaign spawn mask");

    auto* base = memory->base();
    const auto fixture = memory->allocate(0x8000);
    check(fixture != 0, "mission engine fixture allocation failed");
    const auto oldSystem = memory->read32(0x82A690F8);
    const auto oldQuery = PPC_LOOKUP_FUNC(base, queryAddress), oldType = PPC_LOOKUP_FUNC(base, typeAddress),
               oldExecute = PPC_LOOKUP_FUNC(base, executeAddress), oldAssign = PPC_LOOKUP_FUNC(base, assignAddress);
    struct Restore {
        uint8_t* base; uint32_t fixture, oldSystem; PPCFunc *query, *type, *execute, *assign;
        ~Restore() {
            PPC_LOOKUP_FUNC(base, queryAddress) = query;
            PPC_LOOKUP_FUNC(base, typeAddress) = type;
            PPC_LOOKUP_FUNC(base, executeAddress) = execute;
            PPC_LOOKUP_FUNC(base, assignAddress) = assign;
            memory->write32(0x82A690F8, oldSystem);
            memory->release(fixture);
        }
    } restore{base, fixture, oldSystem, oldQuery, oldType, oldExecute, oldAssign};
    const auto client = fixture, systemTable = fixture + 0x3000, consoleTable = fixture + 0x3100;
    systemObject = fixture + 0x3200; console = fixture + 0x3300;
    descriptor = fixture + 0x3400; backing = fixture + 0x3500;
    std::memset(base + fixture, 0, 0x8000);
    memory->write32(client, 0x820807E0);
    memory->write32(client + 516, 0x40); // Main/pause GUI context is accepted.
    memory->write32(client + 7360, fixture + 0x3800);
    memory->write32(systemObject, systemTable); memory->write32(systemTable + 36, queryAddress);
    memory->write32(console, consoleTable); memory->write32(console + 4, 1);
    memory->write32(consoleTable, typeAddress);
    // Check a derived descriptor chain, as the original bridge does.
    memory->write32(descriptor, 0x82096A88); memory->write32(descriptor + 8, descriptor + 16);
    memory->write32(descriptor + 16, 0x82096B8C);
    memory->write32(0x82A690F8, systemObject);
    PPC_LOOKUP_FUNC(base, queryAddress) = query;
    PPC_LOOKUP_FUNC(base, typeAddress) = type;
    PPC_LOOKUP_FUNC(base, executeAddress) = execute;
    PPC_LOOKUP_FUNC(base, assignAddress) = assign;
    returnConsole = true; queries = executions = assignments = 0; command.clear();
    check(canLoadDeveloperMission(base, 0), "mission load rejected a ready frontend without a world client");
    check(canLoadDeveloperMission(base, client), "mission load rejected a ready main-menu client");
    memory->write32(client + 516, 0x20);
    check(!canLoadDeveloperMission(base, client), "mission load accepted an active client transition");
    memory->write32(client + 516, 0x40);
    memory->write32(client, 0xDEADBEEF);
    check(!canLoadDeveloperMission(base, client), "mission load accepted an unrelated guest object");
    memory->write32(client, 0x82081BF0);
    std::string status;
    check(!loadDeveloperMission(ctx, base, client, "invalid", status) && queries == 0,
          "unknown mission touched the original console");
    const auto previous = ctx;
    check(loadDeveloperMission(ctx, base, 0, "NY1_Tunnel:ny1+layer1", status),
          "validated frontend mission did not execute without a world client");
    check(command == frontendTunnel && queries == 1 && assignments == 1 && executions == 1,
          "frontend mission did not execute one validated session-init script at the original console boundary");
    check(PPC_LOAD_U16(backing) == 0x4001 && memory->read32(console + 4) == 1,
          "mission bridge leaked or consumed its caller-owned CStr/console references");
    check(std::memcmp(&ctx, &previous, sizeof(ctx)) == 0,
          "mission console execution changed the caller's guest context");
    check(status.find("Loading a mission may autosave") != std::string::npos,
          "mission status concealed original checkpoint effects");
    check(loadDeveloperMission(ctx, base, client, "SUB_Canal:ny2+layer1", status) &&
          command == canal && queries == 2 && assignments == 2 && executions == 2,
          "running-session mission omitted original disconnect/deferred profile initialization");
    check(PPC_LOAD_U16(backing) == 0x4001 && memory->read32(console + 4) == 1 &&
          std::memcmp(&ctx, &previous, sizeof(ctx)) == 0,
          "running-session mission changed caller context or original reference ownership");
    returnConsole = false;
    check(!loadDeveloperMission(ctx, base, client, "SUB_Canal:ny2+layer1", status) && executions == 2,
          "missing original console executed a mission script");
    memory->write32(0x82A690F8, 0);
    check(!canLoadDeveloperMission(base, client) && !canLoadDeveloperMission(base, 0),
          "mission readiness accepted an uninitialized system");
    puts("Developer missions: 65 retail variants, original disconnect/deferred session transitions and CStr/console boundary passed.");
}
