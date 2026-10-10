#include "runtime/native/native_menu_text.h"
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

using namespace DarkRecomp::Native;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        for (const bool revision : {false, true}) {
            for (const uint32_t language : {1u, 3u, 4u, 5u, 6u}) {
                initializeNativeMenuText(revision, language);
                const bool expected = revision && language == 1;
                check(russianNativeMenus() == expected, "Russian menus escaped their verified revision/content slot");
                check(nativeMenuText("ON") == (expected ? "\xc2\xca\xcb" : "ON"),
                      "Cyrillic menu text was not encoded as CP1251");
                check(nativeMenuText("1440P") == "1440P" && nativeMenuText("MSAA 4X") == "MSAA 4X",
                      "technical labels lost their fallback");
            }
        }
        initializeNativeMenuText(true, 1);
        check(nativeMenuText("MOVE FORWARD") == "\xc2\xcf\xc5\xd0\xa8\xc4",
              "Russian Yo did not retain its font glyph byte");
        initializeNativeMenuText(false, 1);
        check(nativeMenuText("MOVE FORWARD") == "MOVE FORWARD", "Russian state survived a stock reload");
        std::puts("Native menu text: verified revision, English content slot, CP1251 and technical fallback passed.");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
