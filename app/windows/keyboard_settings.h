#pragma once
#include "runtime/native/keyboard_bindings.h"
#include <filesystem>

namespace DarkRecomp {
Native::KeyboardBindings loadKeyboardBindings(const std::filesystem::path& path) noexcept;
// Atomically replaces the [Keyboard] selection while preserving other sections.
bool saveKeyboardBindings(const std::filesystem::path& path,
                          const Native::KeyboardBindings& bindings) noexcept;
}
