#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DarkRecomp::Native {
enum class KeyboardAction : uint8_t {
    MoveForward, MoveBackward, MoveLeft, MoveRight,
    LookUp, LookDown, LookLeft, LookRight,
    Jump, Use, Reload, Crouch, Zoom, FireRight, FireLeft,
    PreviousWeapon, NextWeapon, PreviousPower, NextPower,
    ManifestDarkness, UseDarkness, RedirectDarkling, Journal, Pause, Count
};
inline constexpr size_t kKeyboardActionCount = size_t(KeyboardAction::Count);
inline constexpr size_t kKeyboardBindingSlots = 2;
struct KeyboardBindings {
    std::array<std::array<uint16_t, kKeyboardBindingSlots>, kKeyboardActionCount> keys{};
    bool operator==(const KeyboardBindings&) const = default;
};
struct KeyboardActionInfo { const wchar_t* title; const wchar_t* setting; };
const std::array<KeyboardActionInfo, kKeyboardActionCount>& keyboardActions() noexcept;
KeyboardBindings defaultKeyboardBindings() noexcept;
bool isMouseBindingKey(unsigned key) noexcept;
// Keyboard keys and mouse buttons delivered by the game window are assignable. Escape,
// system shortcuts, and the runtime's own function keys stay available.
bool assignableKeyboardKey(unsigned key) noexcept;
bool validKeyboardBindings(const KeyboardBindings& bindings) noexcept;
// A duplicate key swaps with the edited slot, preserving the other action.
bool assignKeyboardKey(KeyboardBindings& bindings, KeyboardAction action, size_t slot,
                       unsigned key) noexcept;
std::wstring keyboardKeyName(unsigned key);
std::string keyboardKeyPrompt(unsigned key);
}
