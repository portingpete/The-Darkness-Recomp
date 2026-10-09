#include "runtime/native/keyboard_menu.h"
#include "runtime/native/native_menu_text.h"
#include <cstdio>
#include <stdexcept>

using namespace DarkRecomp::Native;
namespace {
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
constexpr const char* jump = "darkrecomp.keyboard.bind.8.0";
constexpr const char* use = "darkrecomp.keyboard.bind.9.0";
constexpr const char* save = "darkrecomp.keyboard.save";
constexpr const char* cancel = "darkrecomp.keyboard.cancel";
constexpr const char* status = "darkrecomp.keyboard.status";
void bind(KeyboardMenuState& menu, const char* action, unsigned key) {
    check(menu.action(action), "binding action not recognized");
    check(menu.keyEvent(key, true) && !menu.captureActive(), "fresh key did not complete capture");
    menu.keyEvent(key, false);
}
void activationRelease() {
    KeyboardMenuState menu;
    const auto original = defaultKeyboardBindings();
    check(!menu.keyEvent('E', true), "closed menu consumed gameplay key");
    check(menu.begin(original) && !menu.inputBlocked() && menu.action(jump) && menu.captureActive() && menu.inputBlocked(),
          "cannot start capture or capture did not block menu navigation");
    check(menu.label(jump) == "sc, [RELEASE ]", "activation release label changed native bounds");
    check(menu.keyEvent('E', true, true) && menu.staged() == original, "activation repeat rebound key");
    check(menu.keyEvent('P', true) && menu.staged() == original, "new key accepted before activation release");
    check(menu.keyEvent('E', false) && menu.keyEvent('P', false), "release edges escaped capture");
    check(menu.label(jump) == "sc, [PRESSKEY]", "capture label changed native bounds after release");
    check(menu.keyEvent('M', true) && !menu.captureActive() && !menu.inputBlocked(), "fresh key after release rejected");
    check(menu.staged().keys[size_t(KeyboardAction::Jump)][0] == 'M' && original == defaultKeyboardBindings(),
          "capture did not stage the selected key");
    menu.keyEvent('M', false);
    check(menu.begin(defaultKeyboardBindings()) && menu.staged().keys[size_t(KeyboardAction::Jump)][0] == 'M',
          "page switch reset staged bindings");
    menu.end();
    check(menu.begin(original) && menu.staged() == original, "cancelled session leaked staged key");
    menu.keyEvent(VK_SPACE, true);
    menu.action(jump);
    check(menu.keyEvent(VK_ESCAPE, true) && !menu.captureActive() && !menu.takeCloseRequest(),
          "Escape while awaiting activation release exited the menu");
}
void captureValidation() {
    KeyboardMenuState menu;
    const auto original = defaultKeyboardBindings();
    menu.begin(original); menu.action(jump);
    check(menu.keyEvent('M', true, true) && menu.captureActive() && menu.staged() == original,
          "repeat was accepted as a new binding");
    menu.keyEvent('M', false);
    check(menu.keyEvent('P', true, false, true) && menu.captureActive() && menu.staged() == original,
          "Alt combination rebound key");
    check(menu.label(status).find("RESERVED") != std::string::npos, "invalid key did not explain rejection");
    menu.keyEvent('P', false);
    check(menu.keyEvent(VK_F1, true) && menu.captureActive() && menu.staged() == original,
          "reserved runtime shortcut accepted");
    menu.keyEvent(VK_F1, false);
    check(menu.keyEvent('E', true) && !menu.captureActive() &&
          menu.staged().keys[size_t(KeyboardAction::Jump)][0] == 'E' &&
          menu.staged().keys[size_t(KeyboardAction::Use)][0] == VK_SPACE,
          "duplicate capture did not preserve displaced action");
    menu.keyEvent('E', false);
    bind(menu, use, VK_DELETE);
    check(menu.staged().keys[size_t(KeyboardAction::Use)][0] == 0, "Delete did not clear selected slot");
    menu.action(jump); const auto beforeEscape = menu.staged();
    check(menu.keyEvent(VK_ESCAPE, true) && !menu.captureActive() && menu.staged() == beforeEscape &&
          !menu.takeCloseRequest(), "capture Escape changed bindings or exited");
    menu.keyEvent(VK_ESCAPE, false);
    menu.keyEvent('E', true); menu.action(jump); menu.cancelCapture();
    check(!menu.captureActive() && menu.staged() == beforeEscape, "focus loss discarded staged edits");
    bind(menu, jump, 'M');
    check(menu.action("darkrecomp.keyboard.defaults") && menu.staged() == original,
          "restore defaults was not staged");
    for (const char* malformed : {"darkrecomp.keyboard.bind.24.0", "darkrecomp.keyboard.bind.0.2",
         "darkrecomp.keyboard.bind.-1.0", "darkrecomp.keyboard.bind.1.0.extra",
         "darkrecomp.keyboard.bind..0", "darkrecomp.keyboard.bind.999999999999999999999.0", "unknown"})
        check(!menu.action(malformed) && menu.label(malformed).empty() && menu.staged() == original,
              "malformed binding action accepted");
}
void mouseCapture() {
    const auto original = defaultKeyboardBindings();
    KeyboardMenuState menu;
    check(!menu.keyEvent(VK_LBUTTON, true), "closed menu consumed activation click");
    check(menu.begin(original) && menu.action(jump) && menu.label(jump) == "sc, [RELEASE ]",
          "mouse activation click did not wait for release");
    check(menu.keyEvent(VK_XBUTTON1, true) && menu.staged() == original &&
          menu.keyEvent(VK_LBUTTON, false) && menu.label(jump) == "sc, [RELEASE ]",
          "capture accepted a button before all activation inputs released");
    check(menu.keyEvent(VK_XBUTTON1, false) && menu.label(jump) == "sc, [PRESSKEY]",
          "mouse releases did not arm capture");
    check(menu.label(status).find("MOUSE") != std::string::npos && menu.label(status).size() == 44,
          "capture did not describe mouse input within the authored width");
    check(menu.keyEvent(VK_XBUTTON1, true) && !menu.captureActive() &&
          menu.staged().keys[size_t(KeyboardAction::Jump)][0] == VK_XBUTTON1 &&
          menu.label(jump) == "sc, [   M4   ]", "fresh side button did not complete capture");
    menu.keyEvent(VK_XBUTTON1, false);
    bind(menu, use, VK_LBUTTON);
    check(menu.staged().keys[size_t(KeyboardAction::Use)][0] == VK_LBUTTON &&
          menu.staged().keys[size_t(KeyboardAction::FireRight)][1] == 'E',
          "mouse conflict did not preserve the displaced keyboard action");
    bind(menu, use, VK_XBUTTON1);
    check(menu.staged().keys[size_t(KeyboardAction::Use)][0] == VK_XBUTTON1 &&
          menu.staged().keys[size_t(KeyboardAction::Jump)][0] == VK_LBUTTON,
          "side button conflict did not swap both actions");
    for (const auto button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
        KeyboardMenuState each;
        check(each.begin(original), "cannot begin mouse capture fixture");
        bind(each, jump, button);
        check(each.staged().keys[size_t(KeyboardAction::Jump)][0] == button &&
              validKeyboardBindings(each.staged()) && each.label(jump).find('?') == std::string::npos,
              "mouse button capture or label rejected");
    }
}
void altCapture() {
    const auto original = defaultKeyboardBindings();
    KeyboardMenuState menu;
    check(!menu.keyEvent(VK_MENU, true, false, true) && menu.begin(original) &&
          menu.action(jump) && menu.label(jump) == "sc, [RELEASE ]",
          "held Alt did not wait for release before capture");
    check(menu.keyEvent(VK_MENU, true, true, true) && menu.staged() == original,
          "held Alt repeat was captured");
    check(menu.keyEvent(VK_MENU, false, false, true) && menu.label(jump) == "sc, [PRESSKEY]",
          "Alt release did not arm capture");
    check(menu.keyEvent(VK_MENU, true, false, true) && !menu.captureActive() &&
          menu.staged().keys[size_t(KeyboardAction::Jump)][0] == VK_MENU &&
          menu.label(jump) == "sc, [  ALT   ]", "bare Alt system edge did not complete capture");
    menu.keyEvent(VK_MENU, false, false, true);
    check(menu.action(use) && menu.keyEvent(VK_RETURN, true, false, true) && menu.captureActive() &&
          menu.label(status).find("RESERVED") != std::string::npos,
          "Alt+Enter shortcut was assigned as Enter");
    menu.keyEvent(VK_RETURN, false, false, true);
    menu.cancelCapture();
}
void saveCancelAndCompletion() {
    KeyboardMenuState menu;
    const auto original = defaultKeyboardBindings();
    menu.begin(original); bind(menu, jump, 'M'); const auto edited = menu.staged();
    check(menu.action(cancel) && menu.takeCloseRequest() && !menu.takeCloseRequest(), "Cancel close request replayed");
    check(menu.inputBlocked() && menu.keyEvent(VK_ESCAPE, true) && menu.keyEvent(VK_ESCAPE, false),
          "deferred close allowed an Escape press/release into the underlying menu");
    KeyboardMenuSaveRequest request;
    check(!menu.takeSaveRequest(request) && menu.action(use) && !menu.captureActive(),
          "Cancel saved or closing session accepted new edit");
    menu.end(); menu.begin(original);
    check(menu.staged() == original, "Cancel applied staged changes");
    bind(menu, jump, 'M'); menu.action(save);
    check(menu.saving() && menu.inputBlocked() && menu.takeSaveRequest(request) && request.id && request.bindings == edited &&
          !menu.takeSaveRequest(request), "save snapshot was not queued exactly once");
    check(menu.keyEvent(VK_ESCAPE, true) && menu.keyEvent(VK_ESCAPE, false) && menu.saving(),
          "original Back could cancel a save transaction in flight");
    const auto firstId = request.id;
    menu.action("darkrecomp.keyboard.defaults"); menu.action(cancel); menu.action(use);
    check(menu.staged() == edited && !menu.captureActive() && !menu.takeCloseRequest(),
          "pending transaction allowed edits or premature close");
    menu.reportSave(firstId + 100, true);
    check(menu.saving() && !menu.takeCloseRequest(), "unrelated completion closed menu");
    menu.reportSave(firstId, false);
    check(!menu.saving() && !menu.inputBlocked() && !menu.takeCloseRequest() && menu.staged() == edited &&
          menu.label(status).find("FAILED") != std::string::npos, "failed save lost staged edits or closed menu");
    menu.action(save); check(menu.takeSaveRequest(request) && request.id > firstId, "retry reused save identity");
    menu.reportSave(firstId, true);
    check(menu.saving() && !menu.takeCloseRequest(), "stale successful completion closed retry");
    menu.reportSave(request.id, true);
    check(!menu.saving() && menu.takeCloseRequest() && !menu.takeCloseRequest() && menu.staged() == edited,
          "successful save did not close exactly once");
    check(menu.inputBlocked(), "successful save unblocked input before original deferred close");
    menu.action(use); check(!menu.captureActive(), "saved session accepted edit before original deferred pop");
    menu.end(); menu.begin(original); menu.action(save); check(menu.takeSaveRequest(request), "new session save missing");
    const auto endedId = request.id; menu.end(); menu.begin(edited); menu.action(save);
    check(menu.takeSaveRequest(request) && request.id > endedId, "session reset reused transaction identity");
    menu.reportSave(endedId, true);
    check(menu.saving() && !menu.takeCloseRequest(), "ended session completion affected reopened menu");
    menu.reportSave(request.id, false);
}
void saveButtonFeedback() {
    KeyboardMenuState menu;
    const auto original = defaultKeyboardBindings();
    const auto expect = [&](const char* value) {
        const auto text = menu.label(save);
        check(text == std::string("sc, ") + value && text.size() == 8,
              "Save feedback changed its original four-glyph bounds");
    };
    const auto failSave = [&] {
        KeyboardMenuSaveRequest request;
        check(menu.action(save) && menu.takeSaveRequest(request), "feedback fixture could not queue Save");
        expect("SAVE");
        menu.reportSave(request.id, false);
        expect("FAIL");
    };
    expect("SAVE"); // Inactive labels must not expose a prior failure.
    menu.begin(original); bind(menu, jump, 'M'); const auto edited = menu.staged();
    failSave();
    check(menu.staged() == edited && !menu.inputBlocked() && !menu.takeCloseRequest(),
          "failed Save lost staged edits or stopped original menu navigation");
    failSave(); // Retrying immediately restores SAVE until its own result.
    menu.action("darkrecomp.keyboard.defaults");
    expect("SAVE");
    check(menu.staged() == original, "Defaults did not reset the failed edit");
    failSave(); menu.action(cancel);
    expect("SAVE"); // A deferred close must use the normal button label.
    menu.end(); expect("SAVE");
    menu.begin(original); expect("SAVE");
    failSave(); menu.end(); menu.begin(original);
    expect("SAVE"); // A new session starts without the previous failure.
}
void labelsAndBridge() {
    KeyboardMenuState menu; menu.begin(defaultKeyboardBindings());
    check(menu.label(status).size() == 44, "status label changed authored width");
    constexpr const char* forward = "darkrecomp.keyboard.bind.0.0";
    check(menu.label(forward) == "sc, [   W    ]", "single key did not retain visible sizing edges");
    for (size_t action = 0; action < kKeyboardActionCount; ++action) for (size_t slot = 0; slot < 2; ++slot) {
        const auto identifier = "darkrecomp.keyboard.bind." + std::to_string(action) + "." + std::to_string(slot);
        const auto text = menu.label(identifier);
        check(text.starts_with("sc, [") && text.back() == ']' && text.size() == 14,
              "binding label changed authored five-cell hit width");
        for (char glyph : std::string_view(text).substr(4)) check(glyph < 'a' || glyph > 'z', "binding label not uppercase");
        check(menu.action(identifier) && menu.label(identifier) == "sc, [PRESSKEY]",
              "primary or secondary capture label did not fit its initial native bounds");
        check(menu.keyEvent(VK_ESCAPE, true) && !menu.captureActive(), "label fixture did not cancel capture");
        menu.keyEvent(VK_ESCAPE, false);
    }
    bind(menu, forward, VK_SPACE);
    check(menu.label(forward) == "sc, [ SPACE  ]" && menu.label(jump) == "sc, [   W    ]",
          "single-key to SPACE swap changed either original binding rectangle");
    bind(menu, forward, VK_OEM_1);
    check(menu.label(forward) == "sc, [SEMICOL ]", "long key name exceeded the eight-glyph interior");
    endKeyboardMenu(); cancelKeyboardMenuCapture();
    check(beginKeyboardMenu(defaultKeyboardBindings()) && keyboardMenuAction(jump) && keyboardMenuCaptureActive() &&
          keyboardMenuInputBlocked(),
          "global bridge cannot enter capture");
    check(keyboardMenuKeyEvent('M', true) && !keyboardMenuCaptureActive(), "global bridge did not stage capture");
    keyboardMenuKeyEvent('M', false); keyboardMenuAction(save);
    KeyboardMenuSaveRequest request;
    check(takeKeyboardMenuSaveRequest(request) && request.bindings.keys[size_t(KeyboardAction::Jump)][0] == 'M',
          "host save bridge lost staged binding");
    reportKeyboardMenuSave(request.id, false);
    check(!takeKeyboardMenuCloseRequest() && keyboardMenuLabel(status).find("FAILED") != std::string::npos,
          "host failure bridge did not retain menu");
    keyboardMenuAction(cancel); check(takeKeyboardMenuCloseRequest(), "guest close bridge missing");
    endKeyboardMenu();
}
}
static void russianLabels() {
    initializeNativeMenuText(true, 1);
    KeyboardMenuState menu;
    menu.begin(defaultKeyboardBindings());
    check(menu.label("darkrecomp.keyboard.bind.0.0") == "sc, [   W    ]", "Russian labels changed physical keys");
    check(menu.label("darkrecomp.keyboard.bind.0.1") == "sc, [  \xcd\xc5\xd2   ]",
          "Russian unbound label lost CP1251 or fixed binding width");
    check(menu.label("darkrecomp.keyboard.save") == "sc, \xd1\xce\xd5\xd0\xc0\xcd\xc8\xd2\xdc",
          "Russian live Save label overwrote the translated initial text");
    check(menu.label("darkrecomp.keyboard.status").size() == 44, "Russian status changed authored width");
    menu.action("darkrecomp.keyboard.bind.0.0");
    check(menu.label("darkrecomp.keyboard.bind.0.0").size() == 14, "Russian capture label changed hit width");
    initializeNativeMenuText(false, 1);
}
int main() {
    try {
        activationRelease(); captureValidation(); mouseCapture(); altCapture(); saveCancelAndCompletion(); saveButtonFeedback(); labelsAndBridge();
        russianLabels();
        std::puts("Keyboard/mouse menu: activation release, capture validation, staged edits, Save/Cancel and async completion passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Keyboard menu: %s\n", error.what()); return 1;
    }
}
