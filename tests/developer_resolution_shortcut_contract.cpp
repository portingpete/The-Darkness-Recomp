#include "app/windows/developer_resolution_shortcut.h"
#include <cstdio>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        DeveloperResolutionShortcut shortcut;
        constexpr LPARAM repeated = LPARAM(1) << 30;
        const auto press = [&] { require(shortcut.handle(WM_KEYDOWN, VK_F6, 0), "F6 down was not consumed"); };
        const auto release = [&] { require(shortcut.handle(WM_KEYUP, VK_F6, 0), "F6 up was not consumed"); };

        require(!shortcut.enabled(), "shortcut enabled by default");
        press();
        require(!shortcut.takeToggle(), "disabled press queued a resize");
        shortcut.setEnabled(true);
        require(shortcut.enabled(), "enable did not persist");
        press();
        require(shortcut.handle(WM_KEYDOWN, VK_F6, repeated), "repeat was not consumed");
        require(!shortcut.takeToggle(), "enabling a held key queued a resize");
        release();
        require(!shortcut.takeToggle(), "keyup queued a resize");

        press();
        require(shortcut.takeToggle(), "enabled new press did not queue a resize");
        require(!shortcut.takeToggle(), "request was consumed twice");
        press();
        shortcut.handle(WM_KEYDOWN, VK_F6, repeated);
        require(!shortcut.takeToggle(), "held key repeated a resize");
        release();

        press();
        release();
        press();
        release();
        require(!shortcut.takeToggle(), "two queued presses did not cancel");
        press();
        release();
        require(shortcut.takeToggle(), "odd queued press did not survive");

        press();
        shortcut.setEnabled(false);
        require(!shortcut.takeToggle(), "disable retained a pending resize");
        shortcut.setEnabled(true);
        press();
        require(!shortcut.takeToggle(), "re-enable retriggered a held key");
        release();
        press();
        release();
        require(shortcut.takeToggle(), "new press after re-enable was lost");

        shortcut.handle(WM_KILLFOCUS, 0, 0);
        shortcut.handle(WM_KEYDOWN, VK_F6, repeated);
        require(!shortcut.takeToggle(), "focus recovery accepted a repeated keydown");
        release();
        require(!shortcut.handle(WM_KEYDOWN, VK_F5, 0), "unrelated key was consumed");
        require(!shortcut.handle(WM_SYSKEYDOWN, VK_F6, 0), "Alt+F6 became a new shortcut");
        require(!shortcut.takeToggle(), "unrelated message queued a resize");
        puts("Developer resolution shortcut: disabled by default, key edges/repeats, enable while held, queued parity and cancellation verified.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "DeveloperResolutionShortcutContract: %s\n", error.what());
        return 1;
    }
}
