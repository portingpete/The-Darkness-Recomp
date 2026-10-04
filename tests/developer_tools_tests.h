#pragma once
#include "runtime/native/developer_tools.h"
#include <limits>
#include <set>

static void testDeveloperToolsRequests() {
    using namespace DarkRecomp::Native;
    resetDeveloperTools();
    const auto initial = developerSnapshot();
    check(initial.playerSpeed == 1 && !initial.invincible && !initial.noclip && !initial.hasActivePlayer && !initial.canLoadMission,
          "Developer tools must start neutral and unavailable before engine readiness");
    check(!requestDeveloperSpeed(0) && !requestDeveloperSpeed(4.01f) &&
          !requestDeveloperSpeed(std::numeric_limits<float>::quiet_NaN()) &&
          !requestDeveloperSpeed(std::numeric_limits<float>::infinity()),
          "Invalid speed must not enter engine request state");
    check(!requestDeveloperMission("NY1_Tunnel'); quit(); //") && !requestDeveloperMission(""),
          "Only catalog destinations may enter the console request queue");
    check(developerSnapshot().revision == initial.revision,
          "Rejected requests must leave developer state unchanged");
    check(requestDeveloperSpeed(0.25f) && developerSnapshot().playerSpeed == 0.25f &&
          requestDeveloperSpeed(4) && developerSnapshot().playerSpeed == 4,
          "Speed bounds must remain usable");
    requestDeveloperInvincibility(true);
    check(developerSnapshot().invincible, "Invincibility request must be visible to UI");
    requestDeveloperInvincibility(false);
    check(!developerSnapshot().invincible, "Invincibility must have a reversible requested state");
    requestDeveloperNoclip(true);
    check(developerSnapshot().noclip, "Noclip request must be visible to UI");
    requestDeveloperNoclip(false);
    check(!developerSnapshot().noclip, "Noclip must have a reversible requested state");
    requestDeveloperUnlockDarkness();
    check(developerSnapshot().status == "Darkness ability unlock queued.", "Ability grant must enter the engine queue");
    requestDeveloperMaxDarkness();
    check(developerSnapshot().status == "Maximum Darkness level queued.", "Level grant must enter the engine queue");
    const auto missions = developerMissions();
    check(!missions.empty(), "Developer mission catalog must contain shipped campaign destinations");
    std::set<std::string_view> unique;
    for (const auto& mission : missions) {
        check(!mission.id.empty() && !mission.title.empty() && unique.insert(mission.id).second,
              "Mission choices must have labels and unique IDs");
        check(requestDeveloperMission(mission.id), "Every displayed mission must be accepted by request validation");
    }
    resetDeveloperTools();
    check(!developerSnapshot().noclip && !developerSnapshot().invincible && developerSnapshot().playerSpeed == 1,
          "Reset must clear requested session cheats");
    puts("Developer requests: neutral startup, bounded speed, reversible invincibility/noclip, Darkness grant queue and mission validation passed.");
}
