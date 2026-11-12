#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace DarkRecomp::Native {
enum class MovementStyle : uint32_t { Original, Alternate };
constexpr bool validMovementStyle(MovementStyle style) noexcept { return unsigned(style) <= 1; }
constexpr std::string_view movementStyleName(MovementStyle style) noexcept {
    switch (style) {
    case MovementStyle::Alternate: return "Alternate Movement";
    default: return "Original";
    }
}
MovementStyle movementStyle() noexcept;
uint64_t movementStyleRevision() noexcept;
bool setMovementStyle(MovementStyle style) noexcept;
MovementStyle loadMovementStyle(const std::filesystem::path& path) noexcept;
bool saveMovementStyle(const std::filesystem::path& path, MovementStyle style) noexcept;
void requestMovementStyleSave() noexcept;
bool takeMovementStyleSaveRequest() noexcept;
void reportMovementStyleSave(bool saved) noexcept;
bool movementStyleSaveFailed() noexcept;
}
