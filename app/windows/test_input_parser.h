#pragma once
// Opt-in --test-input line parser. This application's explicit own-process
// test interface only: it maps bounded numeric key codes into the same native
// input adapter exercised by InputContract. No OS-wide injection, SendInput,
// focus-rule change, controller-merge change, or guest-data patch.
// Strict grammar: "<key>" or "<key> <holdMs>", nothing else. Bare numeric
// lines keep historical holds (camera I/J/K/L 2000ms, other menu keys 250ms).
// Anything malformed returns false with outputs untouched, so the caller
// stays blocked on the same unconsumed line without side effects.
#include <windows.h>
#include <cstdlib>
#include <sstream>
#include <string>

inline constexpr ULONGLONG kTestInputMaxHoldMs = 10000;
inline constexpr ULONGLONG kTestInputMenuHoldMs = 250;
inline constexpr ULONGLONG kTestInputCameraHoldMs = 2000;
inline bool isTestInputKey(unsigned key) {
    switch (key) {
        case VK_RETURN: case VK_SPACE: case VK_ESCAPE:
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_TAB: case VK_BACK: case VK_SHIFT: case VK_CONTROL:
        case 'W': case 'A': case 'S': case 'D':
        case 'E': case 'R': case 'F': case 'X': case 'Z': case 'C':
        case 'Q': case 'G':
        case '1': case '2': case '3': case '4':
        case 'I': case 'J': case 'K': case 'L':
            return true;
        default: return false;
    }
}
inline bool parseTestInputKeyLine(const std::string& command, unsigned& key, ULONGLONG& holdMs) {
    if (command.empty() || command.size() > 256) return false;
    std::istringstream fields(command);
    std::string first, second, extra;
    if (!(fields >> first)) return false;
    const bool hasSecond = bool(fields >> second);
    if (fields >> extra) return false;
    if (first.empty() || first.size() > 3) return false;
    for (char c : first) if (c < '0' || c > '9') return false;
    const unsigned parsed = unsigned(std::strtoul(first.c_str(), nullptr, 10));
    if (!parsed || parsed > 255 || !isTestInputKey(parsed)) return false;
    ULONGLONG hold = 0;
    if (hasSecond) {
        if (second.empty() || second.size() > 5) return false;
        for (char c : second) if (c < '0' || c > '9') return false;
        hold = ULONGLONG(std::strtoull(second.c_str(), nullptr, 10));
        if (!hold || hold > kTestInputMaxHoldMs) return false;
    } else {
        const bool cameraKey = parsed == 'I' || parsed == 'J' || parsed == 'K' || parsed == 'L';
        hold = cameraKey ? kTestInputCameraHoldMs : kTestInputMenuHoldMs;
    }
    key = parsed; holdMs = hold;
    return true;
}
