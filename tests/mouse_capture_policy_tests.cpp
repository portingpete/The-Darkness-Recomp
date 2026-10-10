#include "app/windows/mouse_capture_policy.h"
#include <cstdio>
#include <stdexcept>

namespace {
using Action = NativeMouseCapturePolicy::Action;
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
void startupAndGameplay() {
    NativeMouseCapturePolicy policy;
    require(policy.update(false, false, true, true) == Action::None, "Intro video captured the mouse");
    require(policy.update(true, false, true, true) == Action::Release, "Main menu did not release the mouse");
    require(policy.update(false, false, true, true) == Action::None, "Inactive menu pointer inferred gameplay");
    require(policy.update(true, false, true, true) == Action::Release, "Menu animation lost pointer ownership");
    require(policy.update(false, false, true, true) == Action::None, "Loading captured before the gameplay signal");
    require(policy.update(false, true, true, true) == Action::Capture, "Starting gameplay still needs a click");
    require(policy.update(false, true, true, true) == Action::None, "Gameplay recaptured every frame");
    require(policy.update(true, true, true, true) == Action::Release, "Menu pointer did not win over stale gameplay signal");
    require(policy.update(true, false, true, true) == Action::Release, "Open pause menu lost mouse release");
    require(policy.update(false, true, true, true) == Action::Capture, "Resuming gameplay still needs a click");
    NativeMouseCapturePolicy noMenu;
    require(noMenu.update(false, true, true, true) == Action::None, "Gameplay signal captured without a menu transition");
}
void acceptedGameplayLoad() {
    NativeMouseCapturePolicy policy;
    policy.update(true, false, true, true);
    require(policy.update(false, false, true, true, true) == Action::Capture,
            "Accepted gameplay load waited for the original gameplay-client signal");
    require(policy.update(false, false, true, true, true) == Action::None,
            "Accepted load captured the same menu transition twice");
    require(policy.update(false, true, true, true) == Action::None,
            "The later gameplay-client signal recaptured an accepted load");

    NativeMouseCapturePolicy noMenu;
    require(noMenu.update(false, false, true, true, true) == Action::None,
            "Accepted load captured without a preceding menu transition");

    NativeMouseCapturePolicy actualMenu;
    actualMenu.update(true, false, true, true);
    require(actualMenu.update(true, false, true, true, true) == Action::Release,
            "Accepted load overrode a real menu's pointer ownership");
    require(actualMenu.update(true, true, true, true) == Action::Release,
            "A real menu lost pointer ownership to a later gameplay-client signal");
}
void acceptedLoadGuards() {
    NativeMouseCapturePolicy manual;
    manual.update(true, false, true, true);
    manual.cancel();
    require(manual.update(false, false, true, true, true) == Action::None,
            "Accepted load undid an explicit menu release");
    require(manual.update(false, true, true, true) == Action::None,
            "A late gameplay-client signal undid release before an accepted load");

    NativeMouseCapturePolicy loading;
    loading.update(true, false, true, true);
    loading.update(false, false, true, true);
    loading.cancel();
    require(loading.update(false, false, true, true, true) == Action::None,
            "Accepted load undid an explicit release during loading");

    NativeMouseCapturePolicy unavailable;
    unavailable.update(true, false, true, true);
    require(unavailable.update(false, false, false, true, true) == Action::None,
            "Accepted load captured while focus or native input was unavailable");
    require(unavailable.update(false, false, true, true) == Action::None,
            "Focus/panel recovery resurrected an accepted load");
    require(unavailable.update(false, true, true, true) == Action::None,
            "A late gameplay-client signal resurrected an unavailable accepted load");

    NativeMouseCapturePolicy suspended;
    suspended.update(true, false, true, true);
    suspended.suspend();
    require(suspended.update(false, false, true, true, true) == Action::None,
            "Accepted load undid a nested window operation's release");

    NativeMouseCapturePolicy controller;
    controller.update(true, false, true, false);
    require(controller.update(false, false, true, false, true) == Action::None,
            "Controller-accepted load took the desktop mouse");
    require(controller.update(false, false, true, true) == Action::None,
            "A later keyboard/mouse source change resurrected a controller load");
    require(controller.update(false, true, true, true) == Action::None,
            "A late gameplay-client signal resurrected a controller load");
}
void manualRelease() {
    NativeMouseCapturePolicy policy;
    policy.update(true, false, true, true);
    policy.cancel();
    require(policy.update(true, false, true, true) == Action::Release, "Explicit menu release changed menu ownership");
    require(policy.update(false, true, true, true) == Action::None, "Manual release was undone on resume");
    policy.update(true, false, true, true);
    require(policy.update(false, true, true, true) == Action::Capture, "A later menu transition could not capture");
    policy.cancel();
    require(policy.update(false, true, true, true) == Action::None, "F2 release was undone during gameplay");
    policy.update(true, false, true, true);
    policy.update(false, false, true, true);
    policy.cancel();
    require(policy.update(false, true, true, true) == Action::None, "Release during loading survived into gameplay");
}
void unavailableAndController() {
    // The same availability gate covers Alt-Tab, minimize, native panels and
    // failed raw-input registration. Regaining eligibility is not a new menu.
    for (unsigned unavailableAt = 0; unavailableAt != 3; ++unavailableAt) {
        NativeMouseCapturePolicy policy;
        policy.update(true, false, unavailableAt != 0, true);
        policy.update(false, false, unavailableAt != 1, true);
        require(policy.update(false, true, unavailableAt != 2, true) == Action::None,
                "Unavailable transition captured the mouse");
        require(policy.update(false, true, true, true) == Action::None,
                "Focus/panel recovery recaptured an old transition");
        policy.update(true, false, true, true);
        require(policy.update(false, true, true, true) == Action::Capture,
                "A fresh menu transition could not recover after focus/panel loss");
    }
    NativeMouseCapturePolicy controller;
    controller.update(true, false, true, false);
    require(controller.update(false, true, true, false) == Action::None,
            "Controller-started gameplay took the desktop mouse");
    require(controller.update(false, true, true, true) == Action::None,
            "Idle source change resurrected a controller transition");
    controller.update(true, false, true, false);
    controller.update(true, false, true, true);
    require(controller.update(false, true, true, true) == Action::Capture,
            "Keyboard/mouse menu navigation could not reclaim capture");
    NativeMouseCapturePolicy refocusedMenu;
    refocusedMenu.update(true, false, true, true);
    refocusedMenu.cancel();
    refocusedMenu.update(true, false, false, true);
    require(refocusedMenu.update(true, false, true, true) == Action::Release,
            "Refocusing an open menu captured before gameplay");
    require(refocusedMenu.update(false, true, true, true) == Action::Capture,
            "Starting/resuming after refocusing a menu still needs a click");
}
void nestedWindowOperations() {
    NativeMouseCapturePolicy policy;
    policy.update(true, false, true, true);
    policy.update(false, true, true, true);
    policy.suspend();
    require(policy.update(false, true, true, true) == Action::None,
            "Gameplay resize/fullscreen/focus recovery immediately recaptured");
    policy.update(true, false, true, true);
    policy.suspend();
    require(policy.update(true, false, true, true) == Action::Release,
            "Window operation while in a menu captured before gameplay");
    require(policy.update(false, true, true, true) == Action::Capture,
            "Window operation while in a menu cancelled its next Start/resume");
}
}
int main() {
    try {
        startupAndGameplay();
        acceptedGameplayLoad();
        acceptedLoadGuards();
        manualRelease();
        unavailableAndController();
        nestedWindowOperations();
        std::puts("Mouse capture: accepted loads, menu-to-game start/resume, startup/loading, manual release, focus/panels and controller use verified. No desktop input.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MouseCapturePolicyContract: %s\n", error.what());
        return 1;
    }
}
