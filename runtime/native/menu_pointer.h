#pragma once
#include "renderer/engine/display_layout.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

struct PPCContext;
namespace DarkRecomp::Native {
struct MenuPointerDisplay {
    uint32_t imageWidth = 0, imageHeight = 0, outputWidth = 0, outputHeight = 0;
};
struct MenuPointerPosition { int32_t x = 0, y = 0; };
// Original face/model/projection product restricted to the logical UI plane:
// clip X/Y/W = row[0] * logicalX + row[1] * logicalY + row[2].
struct MenuPointerProjection {
    std::array<double, 3> x{}, y{}, w{};
};
class MenuPointerProjectionSession {
public:
    void observe(uint32_t root, uint64_t tick, const MenuPointerProjection& projection, bool worldPlane) noexcept {
        if (worldPlane || root != root_ || !worldPlane_ || tick - tick_ >= 250) {
            root_ = root; tick_ = tick; projection_ = projection; worldPlane_ = worldPlane;
        }
    }
    std::optional<MenuPointerProjection> forRoot(uint32_t root, uint64_t tick) const noexcept {
        if (!root || root != root_ || tick - tick_ >= 250) return {};
        return projection_;
    }
private:
    uint32_t root_ = 0;
    uint64_t tick_ = 0;
    MenuPointerProjection projection_;
    bool worldPlane_ = false;
};
struct MenuPointerPlane {
    double frameWidth = 0, frameHeight = 0, viewportX = 0, viewportY = 0;
    double width = 0, height = 0, size = 0;
    double originX = 0, originY = 0, insetX = 0, insetY = 0;
    bool centered = false, halfOrigin = false;
};
using MenuPointerMatrix = std::array<std::array<double, 4>, 4>;
inline std::optional<MenuPointerProjection> projectMenuPointerPlane(const MenuPointerPlane& plane,
    const MenuPointerMatrix& model, double projectionX, double projectionY,
    double viewportWidth, double viewportHeight) noexcept {
    for (const auto& row : model) for (double value : row) if (!std::isfinite(value)) return {};
    for (double value : {plane.frameWidth, plane.frameHeight, plane.viewportX, plane.viewportY,
         plane.width, plane.height, plane.size, plane.originX, plane.originY, plane.insetX, plane.insetY,
         projectionX, projectionY, viewportWidth, viewportHeight}) if (!std::isfinite(value)) return {};
    if (plane.frameWidth <= 0 || plane.frameHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0 ||
        plane.size <= 0 || plane.insetX >= 1 || plane.insetY >= 1) return {};
    double x = plane.centered ? (plane.width - plane.size) * .5 : 0;
    double y = plane.centered ? (plane.height - plane.size) * .5 : 0;
    if (plane.halfOrigin) x = y = -.5 * plane.size;
    x += plane.size * plane.originX; y += plane.size * plane.originY;
    const double sx = plane.size * (1 - plane.insetX) / 640;
    const double sy = plane.size * (1 - plane.insetY) / 480;
    MenuPointerProjection result;
    result.w = {sx * model[0][2], sy * model[1][2], x * model[0][2] + y * model[1][2] + model[3][2]};
    const double px = projectionX * viewportWidth / plane.frameWidth;
    const double py = projectionY * viewportHeight / plane.frameHeight;
    const double ox = (2 * plane.viewportX + viewportWidth) / plane.frameWidth - 1;
    const double oy = 1 - (2 * plane.viewportY + viewportHeight) / plane.frameHeight;
    result.x = {px * sx * model[0][0] + ox * result.w[0], px * sy * model[1][0] + ox * result.w[1],
        px * (x * model[0][0] + y * model[1][0] + model[3][0]) + ox * result.w[2]};
    result.y = {py * sx * model[0][1] + oy * result.w[0], py * sy * model[1][1] + oy * result.w[1],
        py * (x * model[0][1] + y * model[1][1] + model[3][1]) + oy * result.w[2]};
    return result;
}
// Original82354DE0 draws its20x20 grid in viewport pixels. Its draw2D model
// from823471F8 cancels the original viewport projection, so these authored
// size/margin parameters determine the rendered plane without camera guesses.
inline std::optional<MenuPointerProjection> projectMenuPointerPlane(const MenuPointerPlane& plane) noexcept {
    for (double value : {plane.frameWidth, plane.frameHeight, plane.viewportX, plane.viewportY,
         plane.width, plane.height, plane.size, plane.originX, plane.originY, plane.insetX, plane.insetY})
        if (!std::isfinite(value)) return {};
    if (plane.frameWidth <= 0 || plane.frameHeight <= 0 || plane.width <= 0 ||
        plane.height <= 0 || plane.size <= 0 || plane.insetX >= 1 || plane.insetY >= 1) return {};
    double x = plane.centered ? (plane.width - plane.size) * .5 : 0;
    double y = plane.centered ? (plane.height - plane.size) * .5 : 0;
    if (plane.halfOrigin) x = y = -.5 * plane.size; // original8209DD9C
    x += plane.viewportX + plane.size * plane.originX;
    y += plane.viewportY + plane.size * plane.originY;
    const double spanX = plane.size * (1 - plane.insetX), spanY = plane.size * (1 - plane.insetY);
    return MenuPointerProjection{{2 * spanX / (640 * plane.frameWidth), 0, 2 * x / plane.frameWidth - 1},
        {0, -2 * spanY / (480 * plane.frameHeight), 1 - 2 * y / plane.frameHeight}, {0, 0, 1}};
}

// Presentation uses the renderer's own aspect fit. Black bars are outside the
// interface; the original window tree supplies its logical coordinate extent.
inline std::optional<MenuPointerPosition> mapMenuPointer(MenuPointerDisplay display,
    int32_t x, int32_t y, int32_t logicalWidth, int32_t logicalHeight) noexcept {
    if (logicalWidth <= 0 || logicalHeight <= 0) return {};
    const auto fit = fitDisplay(display.imageWidth, display.imageHeight,
                               display.outputWidth, display.outputHeight);
    if (fit.width <= 0 || fit.height <= 0 || x < fit.x || y < fit.y ||
        x >= fit.x + fit.width || y >= fit.y + fit.height) return {};
    return MenuPointerPosition{
        int32_t(std::floor((float(x) - fit.x) * logicalWidth / fit.width)),
        int32_t(std::floor((float(y) - fit.y) * logicalHeight / fit.height))};
}

// Solve the rendered plane's perspective projection directly. This preserves
// animated rotation/tilt and uses the same presentation fit as ordinary UI.
inline std::optional<MenuPointerPosition> mapMenuPointer(MenuPointerDisplay display,
    int32_t x, int32_t y, int32_t logicalWidth, int32_t logicalHeight,
    const MenuPointerProjection& projection) noexcept {
    if (logicalWidth <= 0 || logicalHeight <= 0) return {};
    const auto fit = fitDisplay(display.imageWidth, display.imageHeight,
                               display.outputWidth, display.outputHeight);
    if (fit.width <= 0 || fit.height <= 0 || x < fit.x || y < fit.y ||
        x >= fit.x + fit.width || y >= fit.y + fit.height) return {};
    for (const auto* row : {&projection.x, &projection.y, &projection.w})
        for (const auto value : *row) if (!std::isfinite(value)) return {};
    const auto& X = projection.x; const auto& Y = projection.y; const auto& W = projection.w;
    const double planeDeterminant = X[0] * (Y[1] * W[2] - Y[2] * W[1]) -
        X[1] * (Y[0] * W[2] - Y[2] * W[0]) + X[2] * (Y[0] * W[1] - Y[1] * W[0]);
    // Logical Y points down; rendered NDC Y points up. Reversed winding is
    // the rear of the face, where its mirrored controls cannot own a click.
    if (!std::isfinite(planeDeterminant) || !(planeDeterminant < 0)) return {};
    const double u = 2.0 * (double(x) - fit.x) / fit.width - 1.0;
    const double v = 1.0 - 2.0 * (double(y) - fit.y) / fit.height;
    const double a = X[0] - u * W[0], b = X[1] - u * W[1], c = u * W[2] - X[2];
    const double d = Y[0] - v * W[0], e = Y[1] - v * W[1], f = v * W[2] - Y[2];
    const double determinant = a * e - b * d;
    const double magnitude = std::abs(a * e) + std::abs(b * d);
    if (!std::isfinite(determinant) || std::abs(determinant) <= magnitude * 1e-12) return {};
    const double logicalX = (c * e - b * f) / determinant;
    const double logicalY = (a * f - c * d) / determinant;
    const double clipW = W[0] * logicalX + W[1] * logicalY + W[2];
    if (!std::isfinite(logicalX) || !std::isfinite(logicalY) || !std::isfinite(clipW) ||
        !(clipW > 0) || logicalX < 0 || logicalY < 0 ||
        logicalX >= logicalWidth || logicalY >= logicalHeight) return {};
    return MenuPointerPosition{int32_t(std::floor(logicalX)), int32_t(std::floor(logicalY))};
}

struct MenuPointerEvents { bool hover = false, activate = false; };
class MenuPointerSession {
public:
    MenuPointerEvents update(uint32_t root, uint64_t epoch, uint64_t movement,
                             uint64_t presses, bool valid) noexcept {
        const bool newContext = root != root_ || epoch != epoch_;
        const MenuPointerEvents events{
            valid && !newContext && (movement != movement_ || presses != presses_),
            valid && !newContext && presses != presses_};
        root_ = root; epoch_ = epoch; movement_ = movement; presses_ = presses;
        return events;
    }
private:
    uint32_t root_ = 0;
    uint64_t epoch_ = 0, movement_ = 0, presses_ = 0;
};

void initializeMenuPointer() noexcept;
void setMenuPointerDisplay(uint32_t imageWidth, uint32_t imageHeight,
                           uint32_t outputWidth, uint32_t outputHeight) noexcept;
bool guestMenuPointerActive() noexcept;
// Retail application ownership: +60 is CWorldData, +3672 is CubeFrontEnd;
// FrontEnd initialization stores the owning application at +24.
uint32_t guestGameplayLoadFrontend(uint8_t* base, uint32_t application) noexcept;
// Visible, enabled descendants distinguish an input menu from the FrontEnd's
// empty HUD/title window. The root itself cannot be a pointer target.
bool menuPointerTreeHasControl(uint8_t* base, uint32_t root);
// Run on the guest UI thread with a live root. Returns the actionable hit, or
// zero for background/hidden/disabled controls. Uses the original tree/handlers.
uint32_t dispatchMenuPointer(PPCContext& ctx, uint8_t* base, uint32_t root,
                             MenuPointerPosition point, bool activate);
}
