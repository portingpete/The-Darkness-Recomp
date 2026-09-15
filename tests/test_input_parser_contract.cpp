#include "app/windows/test_input_parser.h"
#include <cstdio>
#include <stdexcept>
#include <string>
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void requireAccept(const std::string& line, unsigned wantKey, ULONGLONG wantHold) {
    unsigned key = 0xDEAD;
    ULONGLONG hold = 0xDEAD;
    if (!parseTestInputKeyLine(line, key, hold) || key != wantKey || hold != wantHold)
        throw std::runtime_error("accepted line parsed wrong: " + line);
}
static void requireReject(const std::string& line) {
    unsigned key = 0xBE;
    ULONGLONG hold = 0xEF;
    if (parseTestInputKeyLine(line, key, hold))
        throw std::runtime_error("invalid line accepted: " + line);
    require(key == 0xBE && hold == 0xEF, "rejection mutated outputs");
}
int main() {
    try {
        require(kTestInputMaxHoldMs == 10000, "max hold changed");
        require(kTestInputMenuHoldMs == 250, "menu hold changed");
        require(kTestInputCameraHoldMs == 2000, "camera hold changed");
        // Historical bare-key holds.
        requireAccept("32", 32, 250);
        requireAccept("13", 13, 250);
        requireAccept("27", 27, 250);
        requireAccept("37", 37, 250);
        requireAccept("40", 40, 250);
        requireAccept("73", 73, 2000);
        requireAccept("74", 74, 2000);
        requireAccept("75", 75, 2000);
        requireAccept("76", 76, 2000);
        // New gameplay keys keep the menu hold.
        requireAccept("87", 87, 250);
        requireAccept("65", 65, 250);
        requireAccept("83", 83, 250);
        requireAccept("68", 68, 250);
        requireAccept("69", 69, 250);
        requireAccept("82", 82, 250);
        requireAccept("70", 70, 250);
        requireAccept("88", 88, 250);
        requireAccept("90", 90, 250);
        requireAccept("67", 67, 250);
        requireAccept("81", 81, 250);
        requireAccept("71", 71, 250);
        requireAccept("49", 49, 250);
        requireAccept("52", 52, 250);
        requireAccept("9", 9, 250);
        requireAccept("8", 8, 250);
        requireAccept("16", 16, 250);
        requireAccept("17", 17, 250);
        // Explicit bounded holds, including both edges.
        requireAccept("87 1", 87, 1);
        requireAccept("32 10000", 32, 10000);
        requireAccept("73 500", 73, 500);
        requireAccept("  69   2000  ", 69, 2000);
        requireAccept("32 ", 32, 250);
        requireAccept(std::string("32") + std::string(254, ' '), 32, 250);
        // Invalid: bad durations, trailing text, signs, overflow, length.
        requireReject("32 0");
        requireReject("32 10001");
        requireReject("32 100000");
        requireReject("32 -1");
        requireReject("32 1.5");
        requireReject("32 abc");
        requireReject("32 100 200");
        requireReject("87 W");
        requireReject("+32");
        requireReject("-1");
        requireReject("3.5");
        requireReject("32x");
        requireReject("x32");
        requireReject("0x20");
        requireReject("W");
        requireReject("0");
        requireReject("256");
        requireReject("999");
        requireReject("7");
        requireReject("");
        requireReject("   ");
        requireReject("4294967296");
        requireReject(std::string(300, '1'));
        requireReject(std::string("32") + std::string(255, ' '));
        require(!isTestInputKey(0) && !isTestInputKey(255) && !isTestInputKey(7),
                "allowlist admits an unmapped key");
        require(isTestInputKey('W') && isTestInputKey(VK_TAB) && isTestInputKey(VK_CONTROL),
                "allowlist lost a mapped key");
        puts("Test input parser contract: historical holds, explicit 1/10000, "
             "rejected bad/trailing/sign/overflow, unchanged outputs, and "
             "gameplay keys verified against the production parser.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "TestInputParserContract: %s\n", error.what());
        return 1;
    }
}
