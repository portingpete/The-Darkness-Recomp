#include "runtime/native/flashback_layout.h"
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace DarkRecomp::Native;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void near(float a, float b, const char* message) {
    require(std::abs(a - b) < .001f, message);
}
static float pixel(FlashbackTransform transform, float x, uint32_t width) {
    return (transform.scaleX * x + transform.translationX + 1) * float(width) * .5f;
}

int main() {
    try {
        constexpr uint32_t canvas = (853u << 16) | 480u;
        for (const uint32_t width : {1280u, 1720u, 2560u}) {
            const FlashbackTransform original{3.0f / width, -1};
            const auto result = fitFlashbackLayout(2, canvas, {width, 720},
                1.5f, 1.5f, original, 0, 853, 480, 0xffffffff);
            if (width == 1280) {
                require(!result, "Original 16:9 flashback changed");
                near(pixel(original, 853, width), 1279.5f, "Retail half-pixel extent changed");
                continue;
            }
            require(result.has_value(), "Authored ultrawide flashback not recognized");
            // Independent oracle: retain the retail 853*1.5 display width,
            // then split the uncovered pixels evenly on both sides.
            const float expectedMargin = (float(width) - 1279.5f) * .5f;
            near(pixel(result->image, 0, width), expectedMargin, "Image is not centered");
            near(pixel(result->image, 853, width), width - expectedMargin, "Image width changed");
            require(result->image.scaleX == original.scaleX, "Authored display aspect was stretched");
            near(pixel(result->leftBar, 0, width), 0, "Left matte misses viewport edge");
            near(pixel(result->leftBar, 853, width), expectedMargin, "Left matte overlaps or gaps image");
            near(pixel(result->rightBar, 0, width), width - expectedMargin, "Right matte overlaps or gaps image");
            near(pixel(result->rightBar, 853, width), float(width), "Right matte misses viewport edge");
            // Host scale 2 must apply once, after logical frame geometry.
            near(pixel(result->image, 0, width) * 2, expectedMargin * 2, "Scale-2 image offset changed");
            near(pixel(result->rightBar, 853, width) * 2, float(width) * 2, "Scale-2 matte coverage changed");
            const auto repeated = fitFlashbackLayout(2, canvas, {width, 720},
                1.5f, 1.5f, original, 0, 853, 480, 0xffffffff);
            require(repeated->image.translationX == result->image.translationX &&
                repeated->leftBar.scaleX == result->leftBar.scaleX,
                "Repeated original draw compounded centering");
        }
        const FlashbackTransform original{3.0f / 1720, -1};
        for (uint32_t mode : {0u, 1u, 3u})
            require(!fitFlashbackLayout(mode, canvas, {1720, 720}, 1.5f, 1.5f,
                original, 0, 853, 480, 0xffffffff), "Other image mode changed");
        for (uint32_t dimensions : {0u, (640u << 16) | 480u, (853u << 16) | 479u})
            require(!fitFlashbackLayout(2, dimensions, {1720, 720}, 1.5f, 1.5f,
                original, 0, 853, 480, 0xffffffff), "Other authored dimensions changed");
        require(!fitFlashbackLayout(2, canvas, {1024, 768}, 1.6f, 1.6f,
            original, 0, 853, 480, 0xffffffff), "Narrow viewport acquired sidebars");
        require(!fitFlashbackLayout(2, canvas, {1720, 720}, 2.0f, 1.5f,
            original, 0, 853, 480, 0xffffffff), "A different HUD canvas was fitted");
        require(!fitFlashbackLayout(2, canvas, {1720, 720}, 1.5f, 1.5f,
            original, 0, 852, 480, 0xffffffff), "Unexpected input geometry was fitted");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        require(!fitFlashbackLayout(2, canvas, {1720, 720}, 1.5f, 1.5f,
            {nan, -1}, 0, 853, 480, 0xffffffff), "Invalid transform was fitted");

        require(!fitFlashbackLayout(2, canvas, {1720, 720}, 1.5f, 1.5f,
            {std::numeric_limits<float>::max(), -1}, 0, 853, 480, 0xffffffff),
            "Overflowing derived transform was fitted");

        // A translated original canvas must retain its anchor while the clone
        // changes scale. This also verifies the matte shares image vertex alpha.
        constexpr float origin = 23;
        const FlashbackTransform translated{original.scaleX, -1 - origin * original.scaleX};
        const auto translatedResult = fitFlashbackLayout(2, canvas, {1720, 720},
            1.5f, 1.5f, translated, origin, 853, 480, 0x80123456);
        require(translatedResult.has_value(), "Translated canvas not fitted");
        near(pixel(translatedResult->leftBar, origin, 1720), 0, "Translated matte lost its viewport anchor");
        near(pixel(translatedResult->rightBar, origin + 853, 1720), 1720, "Translated right matte lost coverage");
        require(translatedResult->matteColor == 0x80000000, "Matte changed image vertex alpha");
        puts("Flashback layout: retail geometry, centered composition, complete symmetric mattes, scale-2, translated canvas, alpha and source scoping verified.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FlashbackLayoutContract: %s\n", error.what());
        return 1;
    }
}
