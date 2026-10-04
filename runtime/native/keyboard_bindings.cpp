#include "keyboard_bindings.h"
#include <cstdio>

namespace DarkRecomp::Native {
const std::array<KeyboardActionInfo, kKeyboardActionCount>& keyboardActions() noexcept {
    static constexpr std::array<KeyboardActionInfo, kKeyboardActionCount> actions{{
        {L"MOVE FORWARD", L"MoveForward"}, {L"MOVE BACKWARD", L"MoveBackward"},
        {L"MOVE LEFT", L"MoveLeft"}, {L"MOVE RIGHT", L"MoveRight"},
        {L"LOOK UP", L"LookUp"}, {L"LOOK DOWN", L"LookDown"},
        {L"LOOK LEFT", L"LookLeft"}, {L"LOOK RIGHT", L"LookRight"},
        {L"JUMP", L"Jump"}, {L"USE / CONFIRM", L"Use"}, {L"RELOAD", L"Reload"},
        {L"CROUCH", L"Crouch"}, {L"ZOOM", L"Zoom"},
        {L"FIRE RIGHT WEAPON", L"FireRight"}, {L"FIRE LEFT WEAPON", L"FireLeft"},
        {L"PREVIOUS WEAPON", L"PreviousWeapon"}, {L"NEXT WEAPON", L"NextWeapon"},
        {L"PREVIOUS DARKNESS POWER", L"PreviousPower"}, {L"NEXT DARKNESS POWER", L"NextPower"},
        {L"MANIFEST DARKNESS", L"ManifestDarkness"}, {L"USE DARKNESS POWER", L"UseDarkness"},
        {L"REDIRECT DARKLING", L"RedirectDarkling"}, {L"JOURNAL", L"Journal"}, {L"PAUSE", L"Pause"}
    }};
    return actions;
}
KeyboardBindings defaultKeyboardBindings() noexcept {
    KeyboardBindings result;
    constexpr std::array<uint16_t, kKeyboardActionCount> primary{
        'W','S','A','D','I','K','J','L', VK_SPACE,'E','R',VK_CONTROL,VK_SHIFT,
        'X','Z','1','2','3','4','Q','G','F',VK_TAB,VK_RETURN};
    for (size_t i = 0; i < primary.size(); ++i) result.keys[i][0] = primary[i];
    result.keys[size_t(KeyboardAction::Crouch)][1] = 'C';
    return result;
}
bool assignableKeyboardKey(unsigned key) noexcept {
    if (!key) return true; // Explicitly unbound.
    if (key >= '0' && key <= '9') return true;
    if (key >= 'A' && key <= 'Z') return true;
    if (key >= VK_NUMPAD0 && key <= VK_DIVIDE) return true;
    if (key >= VK_F1 && key <= VK_F24)
        return key != VK_F1 && key != VK_F2 && key != VK_F5 && key != VK_F6 && key != VK_F8;
    switch (key) {
    case VK_BACK: case VK_TAB: case VK_RETURN: case VK_SHIFT: case VK_CONTROL:
    case VK_SPACE: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_INSERT: case VK_DELETE:
    case VK_OEM_1: case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_MINUS:
    case VK_OEM_PERIOD: case VK_OEM_2: case VK_OEM_3: case VK_OEM_4:
    case VK_OEM_5: case VK_OEM_6: case VK_OEM_7: case VK_OEM_102:
        return true;
    default: return false;
    }
}
bool validKeyboardBindings(const KeyboardBindings& bindings) noexcept {
    std::array<bool, 256> used{};
    for (const auto& action : bindings.keys) for (const auto key : action) {
        if (!assignableKeyboardKey(key) || (key && used[key])) return false;
        if (key) used[key] = true;
    }
    return true;
}
bool assignKeyboardKey(KeyboardBindings& bindings, KeyboardAction action, size_t slot, unsigned key) noexcept {
    const auto index = size_t(action);
    if (index >= bindings.keys.size() || slot >= kKeyboardBindingSlots || !assignableKeyboardKey(key) ||
        !validKeyboardBindings(bindings)) return false;
    auto& current = bindings.keys[index][slot];
    if (key == current) return true;
    if (key) for (auto& other : bindings.keys) for (auto& assigned : other) {
        if (assigned == key) { assigned = current; current = uint16_t(key); return true; }
    }
    current = uint16_t(key);
    return true;
}
std::wstring keyboardKeyName(unsigned key) {
    if (!key) return L"UNBOUND";
    if (key == VK_CONTROL) return L"CTRL";
    if (key == VK_SHIFT) return L"SHIFT";
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    switch (key) {
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_LEFT: case VK_UP:
    case VK_RIGHT: case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
        scan |= 0x100; break;
    }
    wchar_t name[64]{};
    if (scan) {
        const int length = GetKeyNameTextW(LONG(scan << 16), name, 64);
        if (length) { CharUpperBuffW(name, DWORD(length)); return name; }
    }
    return L"KEY " + std::to_wstring(key);
}
std::string keyboardKeyPrompt(unsigned key) {
    if (!key) return "-";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, char(key));
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return "N" + std::to_string(key - VK_NUMPAD0);
    switch (key) {
    case VK_SPACE: return "Spc"; case VK_RETURN: return "Ent"; case VK_TAB: return "Tab";
    case VK_CONTROL: return "Ct"; case VK_SHIFT: return "Sh"; case VK_BACK: return "Bk";
    case VK_LEFT: return "<"; case VK_RIGHT: return ">"; case VK_UP: return "^"; case VK_DOWN: return "v";
    case VK_HOME: return "Home"; case VK_END: return "End"; case VK_PRIOR: return "PgUp";
    case VK_NEXT: return "PgDn"; case VK_INSERT: return "Ins"; case VK_DELETE: return "Del";
    case VK_ADD: case VK_OEM_PLUS: return "+"; case VK_SUBTRACT: case VK_OEM_MINUS: return "-";
    case VK_MULTIPLY: return "*"; case VK_DIVIDE: return "Div"; case VK_OEM_2: return "Slash";
    case VK_DECIMAL: case VK_OEM_PERIOD: return "."; case VK_OEM_COMMA: return ",";
    case VK_OEM_1: return ";"; case VK_OEM_3: return "`"; case VK_OEM_4: return "[";
    case VK_OEM_5: case VK_OEM_102: return "\\"; case VK_OEM_6: return "]"; case VK_OEM_7: return "'";
    default: return "?";
    }
}
}
