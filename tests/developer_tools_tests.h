#pragma once
#include "runtime/native/developer_tools.h"
#include <limits>
#include <set>

static void testDeveloperToolsRequests() {
    using namespace DarkRecomp::Native;
    setDeveloperToolsVisible(false);
    resetDeveloperTools();
    const auto initial = developerSnapshot();
    check(initial.playerSpeed == 1 && !initial.invincible && !initial.noclip && !initial.hasActivePlayer && !initial.canLoadMission,
          "Developer tools must start neutral and unavailable before engine readiness");
    check(!developerToolsNeedsUpdate(), "Closed neutral developer tools must leave the update boundary idle");
    setDeveloperToolsVisible(true);
    check(developerToolsNeedsUpdate(), "Opening developer tools must arm guest availability inspection");
    resetDeveloperTools();
    check(developerToolsNeedsUpdate(), "Reset must preserve an open panel's availability inspection");
    setDeveloperToolsVisible(false);
    check(!developerToolsNeedsUpdate(), "Closing an idle panel must stop guest availability inspection");
    check(!requestDeveloperSpeed(0) && !requestDeveloperSpeed(4.01f) &&
          !requestDeveloperSpeed(std::numeric_limits<float>::quiet_NaN()) &&
          !requestDeveloperSpeed(std::numeric_limits<float>::infinity()),
          "Invalid speed must not enter engine request state");
    check(!requestDeveloperMission("NY1_Tunnel'); quit(); //") && !requestDeveloperMission(""),
          "Only catalog destinations may enter the console request queue");
    check(developerSnapshot().revision == initial.revision,
          "Rejected requests must leave developer state unchanged");
    check(!developerToolsNeedsUpdate(), "Rejected developer requests must not arm guest inspection");
    check(requestDeveloperSpeed(0.25f) && developerSnapshot().playerSpeed == 0.25f &&
          requestDeveloperSpeed(4) && developerSnapshot().playerSpeed == 4,
          "Speed bounds must remain usable");
    setDeveloperToolsVisible(false);
    check(developerToolsNeedsUpdate(), "Closing the panel must retain requested player overrides");
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
    check(!developerToolsNeedsUpdate(), "Reset must disarm closed tools after clearing overrides and queued requests");
    check(requestDeveloperMission(missions.front().id), "Closed-panel mission request failed");
    setDeveloperToolsVisible(false);
    check(developerToolsNeedsUpdate(), "A queued mission must remain armed after the panel closes");
    resetDeveloperTools();
    requestDeveloperUnlockDarkness();
    check(developerToolsNeedsUpdate(), "A closed-panel Darkness request must arm the update boundary");
    resetDeveloperTools();
    puts("Developer requests: neutral startup, bounded speed, reversible invincibility/noclip, Darkness grant queue and mission validation passed.");
}
