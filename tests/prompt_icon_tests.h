// Production prompt tests use the verified original container layouts, not
// synthetic magic ids. Fixtures build both VirtualXTC2 (vtable 0x82097EF0)
// and older VirtualXTC (0x82097B70) guest memory, resolve the ACTUAL selected
// index, publish owned images, and render keyboard/controller/keyboard.
#include "renderer/engine/prompt_icons.h"
#include "renderer/engine/prompt_origin.h"
#include "renderer/engine/texture_upload.h"
#include <filesystem>
#include <limits>
#include <string>

namespace {
DarkRecomp::ColorImage promptTestImage(uint8_t r, uint8_t g, uint8_t b, uint32_t width = 32, uint32_t height = 32,
                                       uint8_t origin = 0) {
    DarkRecomp::ColorImage image;
    image.width = width; image.height = height; image.sourceCodec = 0;
    image.promptOrigin = origin;
    image.pixels.resize(size_t(width) * height * 4);
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = r; image.pixels[i + 1] = g; image.pixels[i + 2] = b; image.pixels[i + 3] = 255;
    }
    return image;
}
uint8_t promptChannel(const DarkRecomp::ColorImage& image, uint32_t x, uint32_t y, unsigned channel) {
    return image.pixels[(size_t(y) * image.width + x) * 4 + channel];
}
void promptPut32(uint8_t* base, uint32_t address, uint32_t v) {
    base[address] = uint8_t(v >> 24); base[address + 1] = uint8_t(v >> 16);
    base[address + 2] = uint8_t(v >> 8); base[address + 3] = uint8_t(v);
}
// Builds a VirtualXTC2 container with two records: index 0 -> GUI_Button_A,
// index 1 -> GUI_Button_B. Production equation (no extra +4):
// nameptr=BE32(table+52*index+44), count at table.
struct Xtc2Fixture { uint32_t container, table, nameA, nameB; };
Xtc2Fixture buildXtc2(uint8_t* base, uint32_t container, uint32_t table, uint32_t nameA, uint32_t nameB) {
    promptPut32(base, container, 0x82097EF0);
    promptPut32(base, container + 136, table);
    promptPut32(base, table, 2);
    promptPut32(base, table + 52u * 0 + 44, nameA);
    promptPut32(base, table + 52u * 1 + 44, nameB);
    const char* a = "GUI_Button_A";
    const char* b = "GUI_Button_B";
    for (unsigned i = 0; a[i]; ++i) base[nameA + i] = uint8_t(a[i]);
    base[nameA + 12] = 0;
    for (unsigned i = 0; b[i]; ++i) base[nameB + i] = uint8_t(b[i]);
    base[nameB + 12] = 0;
    return {container, table, nameA, nameB};
}
struct XtcFixture { uint32_t container, collection, pointers, record0, record1; };
XtcFixture buildXtc(uint8_t* base, uint32_t container, uint32_t collection, uint32_t pointers,
                    uint32_t record0, uint32_t record1) {
    promptPut32(base, container, 0x82097B70);
    promptPut32(base, container + 56, collection);
    promptPut32(base, collection + 4, 2);
    promptPut32(base, collection + 24, pointers);
    promptPut32(base, pointers, record0);
    promptPut32(base, pointers + 4, record1);
    const char* a = "GUI_Button_X";
    const char* b = "GUI_Button_Y";
    for (unsigned i = 0; a[i]; ++i) base[record0 + 24 + i] = uint8_t(a[i]);
    base[record0 + 24 + 12] = 0;
    for (unsigned i = 0; b[i]; ++i) base[record1 + 24 + i] = uint8_t(b[i]);
    base[record1 + 24 + 12] = 0;
    return {container, collection, pointers, record0, record1};
}
}

static void testPromptTileLayout();
static void testPromptIcons() {
    using namespace DarkRecomp::Prompts;
    require(originFromName("GUI_Button_A") == Origin::A, "XTC A did not classify");
    require(originFromName("GUI_Button_DRL") == Origin::DRL, "Paired DRL lost");
    require(originFromName("GUI_Button_DUD") == Origin::DUD, "Paired DUD lost");
    require(originFromName("GUI_Button_L") == Origin::LeftStick, "Stick motion L lost");
    require(originFromName("GUI_Button_LC") == Origin::LS, "Stick click LC lost");
    require(originFromName("GUI_Arrow_Left") == Origin::Unknown, "Decorative arrow classified");
    require(originFromName("GUI_ArrowRight") == Origin::Unknown, "Decorative arrow classified");
    require(buttonFromOrigin(Origin::DRL) == Button::DRL, "DRL button lost pairing");
    require(buttonFromOrigin(Origin::LeftStick) == Button::LeftStick, "Motion/click conflated");
    // Live look-tutorial regression (boot-20260909-185242 input-10/11): the
    // "Use X to look around" prompt showed WASD-only. CubeWnd.xcr proves
    // 360_S/SLR/SUD serve BOTH move and look (and 360_C both crouch and aim),
    // so they must never resolve to a single-side meaning in any context.
    require(originFromName("GUI_Button_360_S") == Origin::MoveLook, "360_S misclassified as single stick");
    require(originFromName("GUI_Button_360_SLR") == Origin::MoveLook, "360_SLR misclassified as single stick");
    require(originFromName("GUI_Button_360_SUD") == Origin::MoveLook, "360_SUD misclassified as single stick");
    require(originFromName("GUI_Button_360_C") == Origin::CrouchAim, "360_C misclassified as single click");
    require(buttonFromOrigin(Origin::MoveLook) == Button::MoveLook, "MoveLook button lost");
    require(buttonFromOrigin(Origin::CrouchAim) == Button::CrouchAim, "CrouchAim button lost");
    require(buttonFromOrigin(Origin::MoveLook) != Button::LeftStick, "Look tutorial would show WASD-only again");
    require(buttonFromOrigin(Origin::MoveLook) != Button::RightStick, "Move tutorial would show mouse-only");
    require(buttonFromOrigin(Origin::CrouchAim) != Button::LS, "Aim tutorial would show Ctrl-only");
    require(buttonFromOrigin(Origin::CrouchAim) != Button::RS, "Crouch tutorial would show Shift-only");
    require(originFromName("GUI_Button_L_LR") == Origin::LeftStick, "Side-specific L_LR moved");
    require(originFromName("GUI_Button_R_UD") == Origin::RightStick, "Side-specific R_UD moved");
    // Truthful combined labels: mouse capture proves nothing, so ambiguous
    // buttons show both bindings in every context.
    require(std::string(keyboardLabel(Button::B, Context::Menu)) == "R/Esc", "B label is not combined R/Esc");
    require(std::string(keyboardLabel(Button::B, Context::Gameplay)) == "R/Esc", "Gameplay B label is not combined");
    require(std::string(keyboardLabel(Button::DLeft, Context::Menu)) == "1/<", "DLeft label is not combined");
    require(std::string(keyboardLabel(Button::DRL, Context::Menu)) == "1/2", "Paired DRL label lost");
    require(std::string(keyboardLabel(Button::DUD, Context::Gameplay)) == "3/4", "Paired DUD label lost");
    require(std::string(keyboardLabel(Button::A, Context::Gameplay)) == "E", "Prompt A label is not E");
    require(std::string(keyboardLabel(Button::X, Context::Gameplay)) == "F", "Prompt X label is not F");
    require(std::string(keyboardLabel(Button::Y, Context::Menu)) == "Spc", "Prompt Y label is not Spc");
    require(std::string(keyboardLabel(Button::LB, Context::Gameplay)) == "Q", "Prompt LB label is not Q");
    require(std::string(keyboardLabel(Button::RB, Context::Gameplay)) == "G", "Prompt RB label is not G");
    require(std::string(keyboardLabel(Button::LT, Context::Gameplay)) == "RMB", "Prompt LT label is not RMB");
    require(std::string(keyboardLabel(Button::RT, Context::Gameplay)) == "LMB", "Prompt RT label is not LMB");
    require(std::string(keyboardLabel(Button::Start, Context::Menu)) == "Ent", "Prompt Start label is not Ent");
    require(std::string(keyboardLabel(Button::Back, Context::Menu)) == "Tab", "Prompt Back label is not Tab");
    require(std::string(keyboardLabel(Button::LeftStick, Context::Gameplay)) == "WASD", "Left stick label is not WASD");
    require(std::string(keyboardLabel(Button::RightStick, Context::Gameplay)) == "Mse", "Right stick label is not Mse");
    require(std::string(keyboardLabel(Button::MoveLook, Context::Menu)) == "WASD/Mse", "MoveLook label is not combined");
    require(std::string(keyboardLabel(Button::MoveLook, Context::Gameplay)) == "WASD/Mse", "Look context lost the mouse half");
    require(std::string(keyboardLabel(Button::CrouchAim, Context::Menu)) == "Ct/Sh", "CrouchAim label is not combined");

    const DarkRecomp::ColorImage key = makeKeycap("E");
    require(key.valid() && key.width == 32 && key.height == 32, "Keycap must stay 32x32 for square quads");
    require(promptChannel(key, 0, 0, 3) == 0, "Keycap corner is not transparent");
    require(promptChannel(key, 16, 16, 3) == 255, "Keycap face is not opaque");
    bool glyphPixel = false;
    for (uint32_t y = 4; y < 28 && !glyphPixel; ++y)
        for (uint32_t x = 4; x < 28; ++x)
            if (promptChannel(key, x, y, 0) < 80 && promptChannel(key, x, y, 3) == 255) { glyphPixel = true; break; }
    require(glyphPixel, "Keycap label pixels are missing");
    // Golden bytes of the reviewed, resampled Xelu E PNG: prevent silently
    // falling back to the old procedural artwork or swapping RGBA channels.
    uint32_t keyHash = 2166136261u;
    for (uint8_t byte : key.pixels) keyHash = (keyHash ^ byte) * 16777619u;
    require(keyHash == 0xAB42DF67u, "Keyboard prompt is not the reviewed Xelu Light E artwork");
    require(!makeKeycap(nullptr).valid() && !makeKeycap("unmapped").valid(),
            "Unknown prompt must fail closed");
    for (const auto& ref : kTextureRefs) {
        for (Context context : {Context::Menu, Context::Gameplay}) {
            const auto mapped = iconFor(ref.button, context);
            require(mapped && mapped->valid(), "Verified prompt has no Xelu Light artwork");
            bool visible = false;
            for (size_t i = 3; i < mapped->pixels.size(); i += 4)
                visible |= mapped->pixels[i] != 0;
            require(visible, "Verified prompt has empty Xelu Light artwork");
        }
    }
    require(iconFor(Button::RS, Context::Menu)->pixels == makeKeycap("Sh/MMB").pixels,
            "Aim prompt lost its Shift alternative");
    const DarkRecomp::ColorImage combined = makeKeycap("R/Esc");
    require(combined.valid() && combined.width == 32, "Combined keycap must stay 32x32 stacked");
    bool topPixel = false, bottomPixel = false;
    for (uint32_t x = 4; x < 28; ++x)
        for (uint32_t y = 5; y < 12; ++y)
            if (promptChannel(combined, x, y, 0) < 80 && promptChannel(combined, x, y, 3) == 255) topPixel = true;
    for (uint32_t x = 4; x < 28; ++x)
        for (uint32_t y = 18; y < 27; ++y)
            if (promptChannel(combined, x, y, 0) < 120 && promptChannel(combined, x, y, 3) == 255) bottomPixel = true;
    require(topPixel && bottomPixel, "Stacked combined label lost a row");
    const DarkRecomp::ColorImage down = makeKeycap("4/v");
    bool downPixel = false;
    for (uint32_t x = 4; x < 28; ++x)
        for (uint32_t y = 18; y < 27; ++y)
            if (promptChannel(down, x, y, 0) < 80 && promptChannel(down, x, y, 3) == 255) downPixel = true;
    require(downPixel, "Down-arrow row is missing; 'v' must not map to letter V");

    const DarkRecomp::ColorImage left = makeMouseIcon(MouseIcon::Left);
    const DarkRecomp::ColorImage right = makeMouseIcon(MouseIcon::Right);
    const DarkRecomp::ColorImage wheel = makeMouseIcon(MouseIcon::Wheel);
    require(left.valid() && right.valid() && wheel.valid(), "Mouse icon extent is wrong");
    require(promptChannel(left, 0, 0, 3) == 0, "Mouse surround is not transparent");
    require(promptChannel(left, 12, 10, 0) > 180 && promptChannel(left, 12, 10, 1) < 80 &&
            promptChannel(left, 20, 10, 1) > 200,
            "Left mouse highlight is on the wrong button");
    require(promptChannel(right, 20, 10, 0) > 180 && promptChannel(right, 20, 10, 1) < 80 &&
            promptChannel(right, 12, 10, 1) > 200,
            "Right mouse highlight is on the wrong button");
    require(promptChannel(wheel, 24, 7, 3) > 200 && promptChannel(wheel, 24, 23, 3) > 200,
            "Wheel arrow keycaps are missing");

    clearPromptRegistry();
    auto original = std::make_shared<DarkRecomp::ColorImage>(
        promptTestImage(200, 40, 40, 32, 32, uint8_t(Origin::A)));
    require(classifyPromptTexture(7, *original) == Button::A, "Owned origin did not classify");
    const auto icon = replacementFor(7, original, Source::KeyboardMouse, Context::Menu);
    require(icon && icon->valid() && icon != original, "Verified origin was not replaced");
    require(icon->width == 32 && icon->height == 32, "Replacement icon must be 32x32");
    require(replacementFor(7, original, Source::KeyboardMouse, Context::Menu) == icon,
            "Replacement icon was recreated instead of reused");
    require(replacementFor(7, original, Source::Controller, Context::Menu) == nullptr,
            "Controller source did not restore original artwork");
    require(replacementFor(7, original, Source::KeyboardMouse, Context::Menu) == icon,
            "Source switching is not reversible");
    auto unrelated = std::make_shared<DarkRecomp::ColorImage>(promptTestImage(40, 40, 200));
    require(classifyPromptTexture(7, *unrelated) == Button::Unknown, "Unknown origin classified");
    require(replacementFor(7, unrelated, Source::KeyboardMouse, Context::Menu) == nullptr,
            "Unrelated texture was replaced");
    auto cube = std::make_shared<DarkRecomp::ColorImage>(promptTestImage(200, 40, 40, 32, 32, uint8_t(Origin::A)));
    cube->faces = 6;
    cube->pixels.resize(size_t(32) * 32 * 6 * 4, 128);
    require(replacementFor(7, cube, Source::KeyboardMouse, Context::Menu) == nullptr,
            "Cubemap prompt was replaced");
    // Same engine id reused for another origin must not return stale icons.
    auto reused = std::make_shared<DarkRecomp::ColorImage>(promptTestImage(40, 200, 40, 32, 32, uint8_t(Origin::B)));
    const auto iconB = replacementFor(7, reused, Source::KeyboardMouse, Context::Menu);
    require(iconB && iconB != icon, "Texture-id reuse returned a stale classification");
    testPromptTileLayout();
    clearPromptRegistry();
    std::puts("PromptIcons passed: Xelu Light art, complete mappings, combined labels, alpha, mouse, owned switching and id reuse.");
}

static void testPromptTileLayout() {
    using namespace DarkRecomp::Prompts;
    // Tile math: full-bleed detection, half-tile rects, clamping, degenerate.
    require(promptFullBleed(0, 0, 1, 1), "Full-bleed UV rejected");
    require(!promptFullBleed(0.5f, 0, 1, 1), "Right-half tile read as full-bleed");
    require(!promptFullBleed(0, 0, 0.75f, 1), "Partial tile read as full-bleed");
    TileRect full = promptTileRect(0, 0, 1, 1, 64, 32);
    require(full.valid && full.x == 0 && full.y == 0 && full.w == 64 && full.h == 32, "Full tile rect wrong");
    TileRect half = promptTileRect(0.5f, 0, 1, 1, 64, 32);
    require(half.valid && half.x == 32 && half.y == 0 && half.w == 32 && half.h == 32, "Right-half tile rect wrong");
    TileRect clamp = promptTileRect(-0.25f, -0.5f, 1.5f, 2, 64, 32);
    require(clamp.valid && clamp.x == 0 && clamp.y == 0 && clamp.w == 64 && clamp.h == 32, "Clamped tile rect wrong");
    // Boundary: wholly off-canvas, non-finite, degenerate and empty ranges
    // are invalid so the resolver keeps the original artwork.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    require(!promptTileRect(-3, -2, -1, -0.5f, 64, 32).valid, "Wholly negative UV classified");
    require(!promptTileRect(2, 1.5f, 3, 2, 64, 32).valid, "Wholly >1 UV classified");
    require(!promptTileRect(nan, 0, 1, 1, 64, 32).valid, "NaN UV classified");
    require(!promptTileRect(0, 0, 1, inf, 64, 32).valid, "Inf UV classified");
    require(!promptFullBleed(nan, 0, 1, 1), "NaN UV read as full-bleed");
    require(!promptFullBleed(0, 0, 1, inf), "Inf UV read as full-bleed");
    require(!promptTileRect(0.5f, 0, 0.5f, 1, 64, 32).valid, "Degenerate edge UV classified");
    require(!promptTileRect(0, 0, 1, 1, 0, 32).valid, "Zero-width canvas classified");
    TileRect reversed = promptTileRect(1, 1, 0, 0, 64, 32);
    require(reversed.valid && reversed.x == full.x && reversed.y == full.y &&
            reversed.w == full.w && reversed.h == full.h, "Reversed valid bounds differ");
    // Demonstrated look-tutorial canvas: 64x32 origin art must keep size and
    // aspect (no stretch, no crop of the replacement).
    auto wide = std::make_shared<DarkRecomp::ColorImage>(
        promptTestImage(20, 20, 20, 64, 32, uint8_t(Origin::MoveLook)));
    // Hand-built invalid tiles never index out of bounds.
    TileRect bad;
    bad.x = 1000; bad.y = 0; bad.w = 64; bad.h = 32; bad.valid = true;
    require(!composePromptIcon(*wide, Button::A, Context::Menu, bad).valid(),
            "Out-of-canvas tile composed");
    TileRect empty;
    empty.valid = true;
    require(!composePromptIcon(*wide, Button::A, Context::Menu, empty).valid(),
            "Empty tile composed");
    const auto wideIcon = replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 0, 0, 1, 1);
    require(wideIcon && wideIcon->valid(), "Wide prompt was not replaced");
    require(wideIcon->width == 64 && wideIcon->height == 32, "Wide canvas size changed");
    require(wideIcon->pixels[(size_t(4) * 64 + 4) * 4 + 3] == 0, "Transparent pillar lost");
    // Compare the full artwork, including the gap between the authored WASD
    // keycaps and mouse. A single center texel can legitimately be transparent.
    const auto baseIcon = iconFor(Button::MoveLook, Context::Menu);
    require(baseIcon && baseIcon->valid(), "Base MoveLook icon missing");
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x)
            for (unsigned c = 0; c < 4; ++c)
                require(promptChannel(*wideIcon, 16 + x, y, c) == promptChannel(*baseIcon, x, y, c),
                        "Wide Xelu artwork was stretched, shifted or lost alpha");
    // Atlas tile: right-half UVs of the same canvas keep the icon in-tile.
    const auto tiled = replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 0.5f, 0, 1, 1);
    require(tiled && tiled->valid() && tiled->width == 64 && tiled->height == 32, "Tiled replacement lost canvas");
    require(tiled->pixels[(size_t(16) * 64 + 8) * 4 + 3] == 0, "Tile leaked outside its rect");
    for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x)
            for (unsigned c = 0; c < 4; ++c)
                require(promptChannel(*tiled, 32 + x, y, c) == promptChannel(*baseIcon, x, y, c),
                        "Tiled Xelu artwork did not preserve its placement and alpha");
    require(tiled != wideIcon, "Full and tile substitutions shared one image");
    require(replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 0.5f, 0, 1, 1) == tiled,
            "Same tile key was recreated instead of reused");
    require(replacementForUv(11, wide, Source::Controller, Context::Menu, 0, 0, 1, 1) == nullptr,
            "Tiled controller source did not restore original artwork");
    // Degenerate ranges keep the original instead of substituting.
    require(replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, -3, -2, -1, -0.5f) == nullptr,
            "Wholly negative UV substituted");
    require(replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 2, 1.5f, 3, 2) == nullptr,
            "Wholly >1 UV substituted");
    require(replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, nan, 0, 1, 1) == nullptr,
            "NaN UV substituted");
    require(replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 0, 0, 1, inf) == nullptr,
            "Inf UV substituted");
    require(replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 0.5f, 0, 0.5f, 1) == nullptr,
            "Degenerate edge UV substituted");
    // Reversed valid bounds resolve identically (and reuse the same entry).
    const auto reversedIcon = replacementForUv(11, wide, Source::KeyboardMouse, Context::Menu, 1, 1, 0, 0);
    require(reversedIcon == wideIcon, "Reversed bounds did not reuse the full entry");
    // Cache keys compare exact fields: 2112 masks to 64 under the old packing
    // but must never collide, nor may distinct tiles/rects share entries.
    IconKey key64 = Registry::iconKey(Button::MoveLook, Context::Menu, 64, 32, full);
    IconKey key2112 = Registry::iconKey(Button::MoveLook, Context::Menu, 2112, 32, full);
    require(!(key64 == key2112), "64 vs 2112 widths share a cache key");
    IconKey keyHalf = Registry::iconKey(Button::MoveLook, Context::Menu, 64, 32, half);
    require(!(key64 == keyHalf), "Full and tile rects share a cache key");
    TileRect shortTile = half; shortTile.h = 16;
    IconKey keyShort = Registry::iconKey(Button::MoveLook, Context::Menu, 64, 32, shortTile);
    require(!(keyHalf == keyShort), "Tile heights share a cache key");
    require(key64 == Registry::iconKey(Button::MoveLook, Context::Menu, 64, 32, full), "Same key not equal");
    std::puts("PromptTile passed: full/tile/clamp rects, boundaries, exact keys, wide-canvas fit, in-tile composition and aspect.");
}

static void testPromptOriginFixture(DarkRecomp::Native::Memory& memory) {
    using namespace DarkRecomp::Prompts;
    using DarkRecomp::Native::TextureUpload;
    uint8_t* base = memory.base();
    const uint32_t region = memory.allocate(0x10000);
    require(region != 0, "Prompt origin fixture allocation failed");
    const uint32_t container = region, table = region + 0x1000;
    const uint32_t nameA = region + 0x2000, nameB = region + 0x2100;
    const uint32_t container2 = region + 0x3000, collection = region + 0x4000, pointers = region + 0x4100;
    const uint32_t record0 = region + 0x4200, record1 = region + 0x4300;
    std::memset(base + region, 0, 0x10000);
    buildXtc2(base, container, table, nameA, nameB);
    buildXtc(base, container2, collection, pointers, record0, record1);
    char name[64]{};
    require(resolveXtc2Origin(base, container, 1, name) == uint8_t(Origin::B), "XTC2 actual index 1 is not B");
    require(std::string(name) == "GUI_Button_B", "XTC2 actual name is not GUI_Button_B");
    require(resolveXtc2Origin(base, container, 0) == uint8_t(Origin::A), "XTC2 actual index 0 is not A");
    require(resolveXtc2Origin(base, container, 2) == 0, "Out-of-range XTC2 index classified");
    require(resolveXtc2Origin(base, container2, 0) == 0, "Wrong-vtable XTC2 container classified");
    promptPut32(base, table, 0);
    require(resolveXtc2Origin(base, container, 0) == 0, "Empty XTC2 table classified");
    promptPut32(base, table, 2);
    // Truly unterminated: fill the entire 64-byte bounded region with non-NUL.
    for (unsigned i = 0; i < 64; ++i) base[nameA + i] = uint8_t('X');
    require(resolveXtc2Origin(base, container, 0) == 0, "Unterminated XTC2 name classified");
    for (unsigned i = 0; i < 12; ++i) base[nameA + i] = uint8_t("GUI_Button_A"[i]);
    base[nameA + 12] = 0;
    for (unsigned i = 13; i < 64; ++i) base[nameA + i] = 0;
    require(resolveXtc2Origin(base, container, 0) == uint8_t(Origin::A), "Restored XTC2 name lost");
    require(resolveXtcOrigin(base, container2, 0) == uint8_t(originFromName("GUI_Button_X")),
            "Older container index 0 is not X");
    require(resolveXtcOrigin(base, container2, 1) == uint8_t(originFromName("GUI_Button_Y")),
            "Older container index 1 is not Y");
    require(resolveXtcOrigin(base, container2, 2) == 0, "Out-of-range older index classified");
    require(resolveXtcOrigin(base, container, 0) == 0, "Wrong-vtable older container classified");
    for (unsigned i = 0; i < 32; ++i) base[record0 + 24 + i] = uint8_t('Y');
    require(resolveXtcOrigin(base, container2, 0) == 0, "Unbounded older inline name classified");
    const char* xr = "GUI_Button_X";
    for (unsigned i = 0; xr[i]; ++i) base[record0 + 24 + i] = uint8_t(xr[i]);
    base[record0 + 24 + 12] = 0;
    for (unsigned i = 13; i < 32; ++i) base[record0 + 24 + i] = 0;
    // Real collector: sticky conflict, A/B/A never recovers, unknown demotes.
    {
        TextureUpload upload(7, 1, 0, 1);
        upload.notePromptOrigin(uint8_t(Origin::A));
        upload.notePromptOrigin(uint8_t(Origin::A));
        require(upload.promptOrigin() == uint8_t(Origin::A) && !upload.promptConflict(),
                "Agreeing upload origin lost");
        require(upload.promptResult() == uint8_t(Origin::A), "Agreeing upload result lost");
        upload.notePromptOrigin(uint8_t(Origin::B));
        require(upload.promptConflict() && upload.promptOrigin() == 0, "A,B did not latch conflict");
        upload.notePromptOrigin(uint8_t(Origin::A));
        require(upload.promptConflict() && upload.promptResult() == 0, "A,B,A incorrectly recovered A");
    }
    {
        TextureUpload upload(7, 1, 0, 1);
        upload.notePromptOrigin(uint8_t(Origin::A));
        upload.notePromptOrigin(0);
        require(upload.promptConflict() && upload.promptResult() == 0,
                "Unknown mip between known ones was ignored instead of demoting");
        upload.notePromptOrigin(uint8_t(Origin::A));
        require(upload.promptResult() == 0, "Unknown-hole upload recovered");
    }
    {
        // Unknown fresh prefix must never inherit a known seed GUI tag.
        auto seed = std::make_shared<DarkRecomp::ColorImage>();
        seed->width = seed->height = 4; seed->faces = 1;
        seed->promptOrigin = uint8_t(Origin::A);
        seed->authoredMips = true; seed->firstMip = 2; seed->mips.resize(2);
        seed->mips[1].assign(size_t(1) * 1 * 4, 7);
        TextureUpload upload(9, 3, 0, 1, seed, 2);
        require(!upload.promptSeen() && upload.promptResult() == 0,
                "Unverified fresh prefix inherited seed origin");
        upload.notePromptOrigin(uint8_t(Origin::B));
        require(upload.promptResult() == 0, "Conflicting seed/fresh merged");
    }
    {
        // Scoped capture: commits only inside an active scope; consumed origin
        // asserted through the shared take helper and the real observe path.
        using DarkRecomp::Native::previewBeginPromptCapture;
        using DarkRecomp::Native::previewCommitPromptOrigin;
        using DarkRecomp::Native::previewEndPromptCapture;
        using DarkRecomp::Native::previewObserveImage;
        using DarkRecomp::Native::takePromptOriginFor;
        require(!previewCommitPromptOrigin(0x1234, uint8_t(Origin::A)),
                "Unscoped commit left a TLS tag");
        previewBeginPromptCapture(0x1234);
        require(previewCommitPromptOrigin(0x1234, uint8_t(Origin::A)),
                "Scoped commit rejected");
        require(!previewCommitPromptOrigin(0, uint8_t(Origin::A)),
                "Null image commit accepted");
        require(takePromptOriginFor(0x1234) == uint8_t(Origin::A), "Consumed origin is not A");
        require(takePromptOriginFor(0x1234) == 0, "Consume did not clear");
        previewEndPromptCapture();
        previewBeginPromptCapture(0x1234);
        require(previewCommitPromptOrigin(0x1234, 0), "Unknown scoped commit rejected");
        require(takePromptOriginFor(0x1234) == 0, "Unknown consume failed");
        previewEndPromptCapture();
        // No-observe scope leaves nothing for address reuse.
        previewBeginPromptCapture(0x1234);
        require(previewCommitPromptOrigin(0x1234, uint8_t(Origin::A)), "Reuse setup commit rejected");
        previewEndPromptCapture();
        previewBeginPromptCapture(0x1234);
        require(takePromptOriginFor(0x1234) == 0, "Stale origin survived a no-observe scope");
        previewEndPromptCapture();
        // Nesting restores outer through the consume path.
        previewBeginPromptCapture(0xAAAA);
        require(previewCommitPromptOrigin(0xAAAA, uint8_t(Origin::B)), "Outer commit rejected");
        previewBeginPromptCapture(0xBBBB);
        require(previewCommitPromptOrigin(0xBBBB, 0), "Inner unknown commit rejected");
        require(takePromptOriginFor(0xBBBB) == 0, "Inner unknown consume failed");
        previewEndPromptCapture();
        require(takePromptOriginFor(0xAAAA) == uint8_t(Origin::B), "Inner scope did not restore outer origin");
        previewEndPromptCapture();
        // Real observe early returns (inactive, id 0, non-base mip) consume and
        // clear without touching guest memory (base left null).
        previewBeginPromptCapture(0x1234);
        require(previewCommitPromptOrigin(0x1234, uint8_t(Origin::A)), "Observe setup commit rejected");
        previewObserveImage(nullptr, 0x1234, 0, 0);
        previewEndPromptCapture();
        previewBeginPromptCapture(0x1234);
        require(takePromptOriginFor(0x1234) == 0, "id0 early return did not consume");
        previewEndPromptCapture();
        previewBeginPromptCapture(0x5678);
        require(previewCommitPromptOrigin(0x5678, uint8_t(Origin::B)), "Mip setup commit rejected");
        previewObserveImage(nullptr, 0x5678, 1, 1);
        previewEndPromptCapture();
        previewBeginPromptCapture(0x5678);
        require(takePromptOriginFor(0x5678) == 0, "Mip early return did not consume");
        previewEndPromptCapture();
        // Overflow fails closed: depth-8 state survives, inner consumes unknown.
        previewBeginPromptCapture(0x9000);
        require(previewCommitPromptOrigin(0x9000, uint8_t(Origin::A)), "Overflow outer commit rejected");
        for (unsigned i = 1; i < 8; ++i) previewBeginPromptCapture(0x9000 + i);
        previewBeginPromptCapture(0x9008);
        require(!previewCommitPromptOrigin(0x9008, uint8_t(Origin::B)),
                "Overflow commit stored a tag");
        require(takePromptOriginFor(0x9008) == 0, "Overflow consume is not unknown");
        previewEndPromptCapture();
        for (unsigned i = 0; i < 7; ++i) previewEndPromptCapture();
        require(takePromptOriginFor(0x9000) == uint8_t(Origin::A),
                "Overflow clobbered the saved depth-8 origin");
        previewEndPromptCapture();
        require(!previewCommitPromptOrigin(0x9008, uint8_t(Origin::A)),
                "Post-overflow unscoped commit left a tag");
        previewBeginPromptCapture(0x1234);
        require(previewCommitPromptOrigin(0x1234, uint8_t(Origin::A)), "Post-overflow scope broken");
        require(takePromptOriginFor(0x1234) == uint8_t(Origin::A), "Post-overflow consume broken");
        previewEndPromptCapture();
    }
    memory.release(region);
    std::puts("PromptOrigin fixture passed: both layouts, actual index, unknown/bad bounds, sticky A/B/A, seed merge and scope reuse.");
}

static void testPromptIconPreview(DarkRecomp::EnginePreviewD3D11& renderer, const std::filesystem::path& path) {
    using namespace DarkRecomp::Prompts;
    std::error_code status;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), status);
    std::vector<DarkRecomp::SimpleMesh> meshes;
    std::vector<std::shared_ptr<const DarkRecomp::ColorImage>> icons{
        std::make_shared<const DarkRecomp::ColorImage>(makeKeycap("E")),
        std::make_shared<const DarkRecomp::ColorImage>(makeKeycap("R/Esc")),
        std::make_shared<const DarkRecomp::ColorImage>(makeKeycap("Spc")),
        std::make_shared<const DarkRecomp::ColorImage>(makeKeycap("Ent")),
        std::make_shared<const DarkRecomp::ColorImage>(makeMouseIcon(MouseIcon::Left)),
        std::make_shared<const DarkRecomp::ColorImage>(makeMouseIcon(MouseIcon::Right)),
        std::make_shared<const DarkRecomp::ColorImage>(makeMouseIcon(MouseIcon::Wheel)),
        std::make_shared<const DarkRecomp::ColorImage>(makeKeycap("WASD")),
    };
    for (size_t slot = 0; slot < icons.size(); ++slot) {
        DarkRecomp::SimpleMesh mesh;
        mesh.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float x0 = -1.0f + float(slot) * 0.25f, x1 = x0 + 0.22f;
        mesh.vertices = {
            {{x0, -0.35f, 0.5f}, {0, 1}, {1, 1, 1, 1}}, {{x1, -0.35f, 0.5f}, {1, 1}, {1, 1, 1, 1}},
            {{x1, 0.35f, 0.5f}, {1, 0}, {1, 1, 1, 1}}, {{x0, 0.35f, 0.5f}, {0, 0}, {1, 1, 1, 1}},
        };
        mesh.indices = {0, 1, 2, 0, 2, 3};
        mesh.colorTexture = icons[slot];
        mesh.textureId = uint32_t(0xB0 + slot);
        mesh.opaque = true;
        meshes.push_back(std::move(mesh));
    }
    renderer.render(meshes);
    renderer.saveBmp(path);
    std::printf("[PromptPreview] Saved keyboard/mouse prompt icons to %s.\n", path.string().c_str());
    // Keyboard/controller/keyboard switching on owned origins, with actual
    // 32x32 draws and an unrelated image that must stay untouched.
    auto originImage = std::make_shared<DarkRecomp::ColorImage>(promptTestImage(200, 40, 40, 32, 32, uint8_t(Origin::A)));
    auto unrelated = std::make_shared<DarkRecomp::ColorImage>(promptTestImage(40, 40, 200));
    const auto keyboardIcon = replacementFor(11, originImage, Source::KeyboardMouse, Context::Menu);
    require(keyboardIcon && keyboardIcon->width == 32, "Keyboard switch did not select a 32px icon");
    require(replacementFor(11, originImage, Source::Controller, Context::Menu) == nullptr,
            "Controller switch did not restore original artwork");
    require(replacementFor(11, originImage, Source::KeyboardMouse, Context::Menu) == keyboardIcon,
            "Keyboard switch is not reversible");
    require(replacementFor(11, unrelated, Source::KeyboardMouse, Context::Menu) == nullptr,
            "Unrelated image changed across source switches");
    // Actual GPU draw of the selected 32px icon: analytically known blank
    // keycap-face texel (away from the E glyph and outline) plus glyph proof,
    // not the original red controller pixels; unrelated blue stays blue.
    // CPU anchors: (6,26) is blank light face, (16,16) is the dark E stroke.
    require(promptChannel(*keyboardIcon, 6, 26, 0) > 200 && promptChannel(*keyboardIcon, 6, 26, 3) == 255,
            "CPU icon face anchor is not the light keycap face");
    require(promptChannel(*keyboardIcon, 16, 16, 0) < 120 && promptChannel(*keyboardIcon, 16, 16, 3) == 255,
            "CPU icon glyph anchor is not the dark letter stroke");
    DarkRecomp::SimpleMesh draw;
    draw.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    draw.vertices = {
        {{-0.5f, -0.5f, 0.5f}, {0, 1}, {1, 1, 1, 1}}, {{0.5f, -0.5f, 0.5f}, {1, 1}, {1, 1, 1, 1}},
        {{0.5f, 0.5f, 0.5f}, {1, 0}, {1, 1, 1, 1}}, {{-0.5f, 0.5f, 0.5f}, {0, 0}, {1, 1, 1, 1}},
    };
    draw.indices = {0, 1, 2, 0, 2, 3};
    draw.opaque = true;
    draw.colorTexture = keyboardIcon; draw.textureId = 11;
    renderer.render({draw});
    // Quad spans framebuffer 16..48; (22,22) maps to a blank face texel under
    // either vertical orientation, since x=6 sits left of the glyph column.
    const uint32_t facePixel = renderer.readPixel(22, 22);
    require(int((facePixel >> 16) & 255) > 150 && int(facePixel & 255) > 150,
            "Actual 32px icon draw face texel is not the light keycap face");
    bool gpuGlyph = false;
    for (uint32_t y = 20; y < 44 && !gpuGlyph; ++y)
        for (uint32_t x = 20; x < 44; ++x) {
            const uint32_t pixel = renderer.readPixel(x, y);
            if (int((pixel >> 16) & 255) < 120) { gpuGlyph = true; break; }
        }
    require(gpuGlyph, "Actual 32px icon draw shows no dark glyph stroke");
    draw.colorTexture = unrelated;
    renderer.render({draw});
    // readPixel packs raw RGBA bytes LE: channel 0 (low byte) is red,
    // channel 2 (bits 16..23) is blue. Unrelated solid is RGB(40,40,200).
    const uint32_t plainPixel = renderer.readPixel(32, 32);
    nearByte(plainPixel, 0, 40);
    nearByte(plainPixel, 1, 40);
    nearByte(plainPixel, 2, 200);
}
