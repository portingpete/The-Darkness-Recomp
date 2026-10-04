#include "runtime/native/menu_pointer.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace DarkRecomp::Native;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        const MenuPointerDisplay wide{1280, 720, 1280, 720};
        auto point = mapMenuPointer(wide, 640, 360, 640, 480);
        require(point && point->x == 320 && point->y == 240, "Cube logical coordinate mapping");
        require(!mapMenuPointer(wide, 1280, 360, 640, 480), "right boundary must stay outside");
        require(!mapMenuPointer(wide, -1, 0, 640, 480), "negative pointer must stay outside");
        const MenuPointerDisplay pillarbox{1280, 720, 1920, 720};
        require(!mapMenuPointer(pillarbox, 319, 360, 640, 480), "pillarbox must not activate menu");
        point = mapMenuPointer(pillarbox, 320, 0, 640, 480);
        require(point && point->x == 0 && point->y == 0, "pillarbox left edge");
        const MenuPointerDisplay letterbox{1280, 720, 1280, 1000};
        require(!mapMenuPointer(letterbox, 640, 139, 640, 480), "letterbox must not activate menu");
        point = mapMenuPointer(letterbox, 640, 500, 640, 480);
        require(point && point->x == 320 && point->y == 240, "letterbox center");
        require(!mapMenuPointer({}, 0, 0, 640, 480), "uninitialized display must be inert");
        require(!mapMenuPointer(wide, 0, 0, 0, 480), "empty original window must be inert");
        const MenuPointerProjection fullPlane{{2.0 / 640, 0, -1}, {0, -2.0 / 480, 1}, {0, 0, 1}};
        point = mapMenuPointer(wide, 640, 360, 640, 480, fullPlane);
        require(point && point->x == 320 && point->y == 240, "front face center inverse");
        const MenuPointerProjection insetPlane{{1.0 / 640, 0, -.5}, {0, -1.0 / 480, .5}, {0, 0, 1}};
        require(!mapMenuPointer(wide, 100, 360, 640, 480, insetPlane), "outside rendered face must not hit logical UI");
        point = mapMenuPointer(wide, 320, 180, 640, 480, insetPlane);
        require(point && point->x == 0 && point->y == 0, "rendered face left/top boundary inverse");
        require(!mapMenuPointer(wide, 960, 360, 640, 480, insetPlane), "rendered face right boundary must stay outside");
        require(!mapMenuPointer(wide, 640, 540, 640, 480, insetPlane), "rendered face bottom boundary must stay outside");
        const MenuPointerProjection tilted{
            {1.0 / 512, 1.0 / 4096, -(320.5 / 512 + 240.5 / 4096)},
            {1.0 / 8192, -1.0 / 256, -(320.5 / 8192 - 240.5 / 256)},
            {1.0 / 2048, 1.0 / 8192, 1}};
        point = mapMenuPointer(wide, 640, 360, 640, 480, tilted);
        require(point && point->x == 320 && point->y == 240, "rotated tilted perspective face inverse");
        const auto projected = [&](const std::array<double, 3>& row) {
            return row[0] * 128.5 + row[1] * 360.5 + row[2];
        };
        const double projectedW = projected(tilted.w);
        const int32_t projectedX = int32_t(std::lround((projected(tilted.x) / projectedW + 1) * 640));
        const int32_t projectedY = int32_t(std::lround((1 - projected(tilted.y) / projectedW) * 360));
        point = mapMenuPointer(wide, projectedX, projectedY, 640, 480, tilted);
        require(point && std::abs(point->x - 128) <= 1 && std::abs(point->y - 360) <= 1,
                "off-center perspective projection must round-trip within one logical pixel");
        auto scaledProjection = tilted;
        for (auto* row : {&scaledProjection.x, &scaledProjection.y, &scaledProjection.w})
            for (auto& value : *row) value *= 1e-8;
        const auto scaledPoint = mapMenuPointer(wide, projectedX, projectedY, 640, 480, scaledProjection);
        require(point && scaledPoint && point->x == scaledPoint->x && point->y == scaledPoint->y,
                "homogeneous projection scale must not change the inverse");
        point = mapMenuPointer(letterbox, 640, 500, 640, 480, tilted);
        require(point && point->x == 320 && point->y == 240, "projected face must preserve letterbox fit");
        require(!mapMenuPointer(pillarbox, 319, 360, 640, 480, tilted), "projected face must exclude pillarbox");
        require(!mapMenuPointer(letterbox, 640, 139, 640, 480, tilted), "projected face must exclude letterbox");
        const MenuPointerProjection rotated{{0, -2.0 / 480, 1}, {-2.0 / 640, 0, 1}, {0, 0, 1}};
        point = mapMenuPointer(wide, 960, 360, 640, 480, rotated);
        require(point && point->x == 320 && point->y == 120, "quarter-turn face inverse");
        const MenuPointerProjection rearFace{{-2.0 / 640, 0, 1}, {0, -2.0 / 480, 1}, {0, 0, 1}};
        require(!mapMenuPointer(wide, 640, 360, 640, 480, rearFace), "rear-facing mirrored controls must be inert");
        const MenuPointerProjection behind{{-1.0 / 320, 0, .125}, {0, -1.0 / 240, 1}, {1.0 / 200, 0, -.5}};
        require(!mapMenuPointer(wide, 640, 360, 640, 480, behind), "face point behind camera must be inert");
        const MenuPointerProjection singular{{1, 0, 0}, {2, 0, 0}, {0, 0, 1}};
        require(!mapMenuPointer(wide, 640, 360, 640, 480, singular), "singular face must be inert");
        const MenuPointerProjection unstable{{1, 1, 0}, {-1, -1 - 1e-14, 1}, {0, 0, 1}};
        require(!mapMenuPointer(wide, 640, 360, 640, 480, unstable), "numerically edge-on face must be inert");
        auto nonfinite = fullPlane;
        nonfinite.x[0] = std::numeric_limits<double>::quiet_NaN();
        require(!mapMenuPointer(wide, 640, 360, 640, 480, nonfinite), "nonfinite face must be inert");
        MenuPointerPlane authoredPlane;
        authoredPlane.frameWidth = authoredPlane.width = 1280;
        authoredPlane.frameHeight = authoredPlane.height = 720;
        authoredPlane.size = 720; authoredPlane.centered = true;
        authoredPlane.originX = .05; authoredPlane.originY = .1;
        authoredPlane.insetX = authoredPlane.insetY = .2;
        const auto pixelPlane = projectMenuPointerPlane(authoredPlane);
        const MenuPointerMatrix draw2D{{{4,0,0,0},{0,4,0,0},{0,0,1,0},{-2560,-1440,2000,1}}};
        const auto cameraPlane = projectMenuPointerPlane(authoredPlane, draw2D, 1000.0/1280, -1000.0/720, 1280, 720);
        require(pixelPlane && cameraPlane, "original authored plane and draw2D camera must produce projections");
        for (unsigned i = 0; i < 3; ++i)
            require(std::abs(pixelPlane->x[i] - cameraPlane->x[i] / cameraPlane->w[2]) < 1e-12 &&
                    std::abs(pixelPlane->y[i] - cameraPlane->y[i] / cameraPlane->w[2]) < 1e-12,
                    "original draw2D model must cancel matching viewport perspective");
        point = mapMenuPointer(wide, 605, 374, 640, 480, *cameraPlane);
        require(point && point->x == 321 && point->y == 251, "authored margins and spans must determine visible hit coordinates");
        authoredPlane.size = 6.5; authoredPlane.centered = false; authoredPlane.halfOrigin = true;
        authoredPlane.originX = authoredPlane.originY = authoredPlane.insetX = authoredPlane.insetY = 0;
        const MenuPointerMatrix worldPlane{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,5,1}}};
        const auto worldProjection = projectMenuPointerPlane(authoredPlane, worldPlane, .6, -1.0666666666666667, 1280, 720);
        require(worldProjection.has_value(), "original world-unit plane must produce camera projection");
        point = mapMenuPointer(wide, 640, 360, 640, 480, *worldProjection);
        require(point && point->x == 320 && point->y == 240, "world-unit plane center inverse");
        require(!mapMenuPointer(wide, 100, 360, 640, 480, *worldProjection), "world-unit size must never be treated as pixels");
        MenuPointerProjectionSession capturedPlane;
        capturedPlane.observe(100, 10, *worldProjection, true);
        capturedPlane.observe(100, 11, *pixelPlane, false); // original thumbnail pass follows the main glyph pass
        auto captured = capturedPlane.forRoot(100, 12);
        point = captured ? mapMenuPointer(wide, 640, 360, 640, 480, *captured) : std::nullopt;
        require(point && point->x == 320 && point->y == 240, "thumbnail pass must not overwrite visible world-plane hit mapping");
        require(!capturedPlane.forRoot(200, 12), "a covered menu must not lend projection to another root");
        require(!capturedPlane.forRoot(100, 260), "expired render projection must not replay into a later menu");
        capturedPlane.observe(200, 261, fullPlane, false);
        captured = capturedPlane.forRoot(200, 262);
        point = captured ? mapMenuPointer(wide, 640, 360, 640, 480, *captured) : std::nullopt;
        require(point && point->x == 320 && point->y == 240, "a new ordinary plane must establish its own hit mapping");
        MenuPointerSession session;
        auto events = session.update(100, 1, 1, 1, true);
        require(!events.hover && !events.activate, "opening click must not pass into next menu");
        events = session.update(100, 1, 2, 1, true);
        require(events.hover && !events.activate, "movement must focus without activation");
        events = session.update(100, 1, 2, 2, true);
        require(events.hover && events.activate, "fresh click must focus and activate");
        events = session.update(100, 1, 2, 2, true);
        require(!events.hover && !events.activate, "held click must not repeat");
        events = session.update(200, 1, 2, 2, true);
        require(!events.hover && !events.activate, "submenu must not replay previous click");
        events = session.update(200, 2, 3, 3, true);
        require(!events.hover && !events.activate, "focus/capture epoch must discard stale clicks");
        events = session.update(200, 2, 4, 4, false);
        require(!events.hover && !events.activate, "unfocused/settings input must be suppressed");
        events = session.update(200, 2, 4, 4, true);
        require(!events.hover && !events.activate, "regained focus must not replay input");
        puts("Menu pointer mapping and event ownership passed.");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
