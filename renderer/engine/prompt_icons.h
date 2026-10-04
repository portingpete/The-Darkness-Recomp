#pragma once
#include "renderer/engine/prompt_origin.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/xelu_light.generated.h"
#include "renderer/engine/prompt_bindings.h"
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Adaptive keyboard/mouse button prompts. Original controller artwork is never
// modified: replacement icons are embedded Xelu Light images returned only
// for owned images carrying a verified GUI_Button_* origin. Unknown origins
// and unrelated textures resolve to nullptr so the caller keeps the original
// pixels untouched. No per-frame guest scanning, sampled hashes, or volatile
// pointer/ID classification caches.
namespace DarkRecomp::Prompts {
enum class Source : uint8_t { KeyboardMouse = 0, Controller = 1 };
enum class Context : uint8_t { Menu = 0, Gameplay = 1 };
enum class Button : uint8_t {
    A, B, X, Y, LB, RB, LT, RT, LS, RS, Start, Back,
    DUp, DDown, DLeft, DRight, DUD, DRL, LeftStick, RightStick,
    MoveLook, CrouchAim, Unknown
};
inline Button buttonFromOrigin(Origin origin) {
    switch (origin) {
    case Origin::A: return Button::A;
    case Origin::B: return Button::B;
    case Origin::X: return Button::X;
    case Origin::Y: return Button::Y;
    case Origin::LB: return Button::LB;
    case Origin::RB: return Button::RB;
    case Origin::LT: return Button::LT;
    case Origin::RT: return Button::RT;
    case Origin::LS: return Button::LS;
    case Origin::RS: return Button::RS;
    case Origin::Start: return Button::Start;
    case Origin::Back: return Button::Back;
    case Origin::DUp: return Button::DUp;
    case Origin::DDown: return Button::DDown;
    case Origin::DLeft: return Button::DLeft;
    case Origin::DRight: return Button::DRight;
    case Origin::DUD: return Button::DUD;
    case Origin::DRL: return Button::DRL;
    case Origin::LeftStick: return Button::LeftStick;
    case Origin::RightStick: return Button::RightStick;
    case Origin::MoveLook: return Button::MoveLook;
    case Origin::CrouchAim: return Button::CrouchAim;
    default: return Button::Unknown;
    }
}
inline Button buttonFromOriginByte(uint8_t origin) {
    return buttonFromOrigin(Origin(origin));
}

// Verified GUI.xtc coverage (456 IMAGEDIRECTORY5 records; CubeWnd.xcr
// BUTTON_DESCRIPTOR maps ICONSURFACE_A/B/X/Y onto GUI_Button_A_32 etc, the
// _32 suffix being a menu-size spelling of these base records). Decorative
// GUI_Arrow_* textures are intentionally absent: they never classify.
struct TextureRef { const char* name; uint32_t xtcIndex; Button button; };
inline constexpr TextureRef kTextureRefs[] = {
    {"GUI_Button_A", 25, Button::A}, {"GUI_Button_Back", 26, Button::Back},
    {"GUI_Button_DDown", 27, Button::DDown}, {"GUI_Button_DLeft", 28, Button::DLeft},
    {"GUI_Button_DRight", 29, Button::DRight}, {"GUI_Button_DUp", 30, Button::DUp},
    {"GUI_Button_DRL", 31, Button::DRL}, {"GUI_Button_DUD", 32, Button::DUD},
    {"GUI_Button_L", 33, Button::LeftStick}, {"GUI_Button_LB", 34, Button::LB},
    {"GUI_Button_LC", 35, Button::LS}, {"GUI_Button_LT", 36, Button::LT},
    {"GUI_Button_R", 37, Button::RightStick}, {"GUI_Button_RB", 38, Button::RB},
    {"GUI_Button_RC", 39, Button::RS}, {"GUI_Button_RT", 40, Button::RT},
    {"GUI_Button_Start", 41, Button::Start}, {"GUI_Button_X", 42, Button::X},
    {"GUI_Button_Y", 43, Button::Y}, {"GUI_Button_B", 58, Button::B},
    {"GUI_Button_L_LR", 237, Button::LeftStick}, {"GUI_Button_L_UD", 238, Button::LeftStick},
    {"GUI_Button_R_LR", 239, Button::RightStick}, {"GUI_Button_R_UD", 240, Button::RightStick},
    {"GUI_Button_360_SLR", 276, Button::MoveLook}, {"GUI_Button_360_S", 277, Button::MoveLook},
    {"GUI_Button_360_C", 278, Button::CrouchAim}, {"GUI_Button_360_SUD", 279, Button::MoveLook},
};
// CubeWnd.xcr LMENU_CONTROLLER evidence (verified, not guessed): MOVE and LOOK
// both bind GUI_Button_360_S, _SLR and _SUD; CROUCH and AIM both bind
// GUI_Button_360_C. Those four textures are action-ambiguous, so they use
// combined labels. GUI_Button_L/R/L_LR/L_UD/R_LR/R_UD stay side-specific:
// no row binds them to the opposite stick's action.

// CONTROLS.md / runtime/native/input.cpp true bindings: A->E, X->F, Y->Space,
// LB->Q, RB->G, LS->Ctrl/C, Start->Enter, Back->Tab, sticks->WASD/mouse,
// LT->RMB, RT->LMB. The owned draw context selects action-only gameplay
// labels or combined menu labels: B is Reload in gameplay, Reload/Esc in
// menus; dpad uses digits in gameplay and includes arrows in menus. Both
// Shift and MMB bind RS during gameplay. DRL/DUD remain paired directions.
// 360_S/SLR/SUD serve both move (WASD) and look (mouse) and 360_C serves both
// crouch (Ctrl) and aim (Shift) per CubeWnd rows, so they also combine.
inline const char* keyboardLabel(Button button, Context context) {
    if (size_t(button) >= BindingLabels{}.size()) return nullptr;
    // Retain the returned C string until the caller's next label lookup.
    thread_local std::shared_ptr<const BindingLabels> snapshot;
    snapshot = bindingLabels(context == Context::Gameplay);
    return (*snapshot)[size_t(button)].c_str();
}
inline Context contextFromMouseLook(bool) { return Context::Menu; }

// Authored CC0 Xelu Light artwork, baked offline. No PNG decoding or file I/O
// occurs while rendering. The existing owned-origin registry caches completed
// canvases and the original controller source remains untouched.
inline ColorImage makeKeycap(const char* label) {
    if (!label) return {};
    for (const auto& sprite : XeluLight::kSprites) {
        if (sprite.label != label) continue;
        ColorImage image;
        image.width = image.height = XeluLight::kSize;
        image.pixels.resize(XeluLight::kPixelCount * 4);
        for (size_t i = 0; i < XeluLight::kPixelCount; ++i) {
            const uint32_t rgba = sprite.rgba[i];
            for (unsigned channel = 0; channel < 4; ++channel)
                image.pixels[i * 4 + channel] = uint8_t(rgba >> (channel * 8));
        }
        return image;
    }
    // User-selected keys are not limited to the shipped prompt artwork.
    // Keep Xelu's existing keycap face and draw a small readable label for
    // arbitrary keys; default controls retain their exact authored sprites.
    ColorImage image = makeKeycap("?");
    constexpr uint16_t letters[]{
        0x7B6D,0x6BAE,0x7927,0x6B6E,0x79A7,0x79A4,0x796F,0x5BED,0x7497,
        0x124F,0x5BAD,0x4927,0x5FED,0x5FFD,0x7B6F,0x7BA4,0x7B7B,0x7BAD,
        0x788F,0x7492,0x5B6F,0x5B6A,0x5BFD,0x5AAD,0x5A92,0x72A7};
    constexpr uint16_t digits[]{0x7B6F,0x2C97,0x62A7,0x628E,0x5BC9,0x798E,0x79EF,0x7249,0x7BEF,0x7BCF};
    const auto glyph = [&](char c) -> uint16_t {
        if (c == 'v') return 0x00AA;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
        if (c >= '0' && c <= '9') return digits[c - '0'];
        switch (c) {
        case '<': return 0x1544; case '>': return 0x4454; case '^': return 0x2A00;
        case '-': return 0x01C0; case '+': return 0x05D0; case '/': return 0x12A4;
        case '\\': return 0x4429; case '.': return 0x0002; case ',': return 0x0014;
        case '[': return 0x6926; case ']': return 0x324B; case ';': return 0x0414;
        case '\'': return 0x2400; case '`': return 0x4400; case '*': return 0x0ABA;
        default: return 0;
        }
    };
    const auto put = [&](unsigned x, unsigned y, uint8_t value) {
        auto* pixel = image.pixels.data() + (size_t(y) * image.width + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = value; pixel[3] = 255;
    };
    constexpr unsigned artScale = XeluLight::kSize / 32;
    for (unsigned y = 4 * artScale; y < 28 * artScale; ++y)
        for (unsigned x = 4 * artScale; x < 28 * artScale; ++x) put(x, y, 235);
    std::string text(label);
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
        if (i == text.size() || (text[i] == '/' && text.size() > 1)) {
            if (i > start) lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    if (lines.empty()) lines.push_back(text);
    if (lines.size() > 4 || std::any_of(lines.begin(), lines.end(), [](const auto& line) { return line.size() > 6; })) return {};
    const unsigned scale = lines.size() <= 2 &&
        std::all_of(lines.begin(), lines.end(), [](const auto& line) { return line.size() <= 3; }) ? 2 * artScale : artScale;
    const unsigned height = unsigned(lines.size()) * (5 * scale + artScale) - artScale;
    unsigned y = (image.height - height) / 2;
    for (const auto& line : lines) {
        const auto count = (std::min)(line.size(), size_t(6));
        unsigned x = (image.width - unsigned(count) * 4 * scale + scale) / 2;
        for (size_t i = 0; i < count; ++i) {
            const uint16_t bits = glyph(line[i]);
            for (unsigned row = 0; row < 5; ++row) for (unsigned col = 0; col < 3; ++col)
                if (bits & (1u << (14 - (row * 3 + col))))
                    for (unsigned dy = 0; dy < scale; ++dy) for (unsigned dx = 0; dx < scale; ++dx)
                        put(x + col * scale + dx, y + row * scale + dy, 36);
            x += 4 * scale;
        }
        y += 5 * scale + artScale;
    }
    return image;
}
enum class MouseIcon : uint8_t { Left, Right, Middle, Wheel };
inline ColorImage makeMouseIcon(MouseIcon icon) {
    switch (icon) {
    case MouseIcon::Left: return makeKeycap("LMB");
    case MouseIcon::Right: return makeKeycap("RMB");
    case MouseIcon::Middle: return makeKeycap("MMB");
    case MouseIcon::Wheel: return makeKeycap("Wheel");
    }
    return {};
}
inline std::shared_ptr<const ColorImage> iconFor(Button button, Context context, const char* configuredLabel = nullptr) {
    const char* label = configuredLabel ? configuredLabel : keyboardLabel(button, context);
    if (!label) return nullptr;
    auto image = makeKeycap(label);
    if (!image.valid()) return nullptr;
    return std::make_shared<const ColorImage>(std::move(image));
}

// Tile-aware composition. Prompt textures are not always square full-bleed
// art: the live look tutorial uploads 64x32 textures, and meshes may sample
// atlas tiles. Double the canvas resolution while retaining normalized tile
// bounds, fit the 64px authored icon into that tile, and generate a complete
// mip chain. Large canvases retain their size within the image limit.
struct TileRect { uint32_t x = 0, y = 0, w = 0, h = 0; bool valid = false; };
inline bool promptFiniteUv(float u) { return u == u && u < 4e18f && u > -4e18f; }
inline bool promptFullBleed(float u0, float v0, float u1, float v1) {
    if (!promptFiniteUv(u0) || !promptFiniteUv(v0) || !promptFiniteUv(u1) || !promptFiniteUv(v1))
        return false;
    constexpr float eps = 1.f / 256;
    return u0 <= eps && v0 <= eps && u1 >= 1 - eps && v1 >= 1 - eps;
}
// Bounds-checked tile classification. Both endpoints are clamped fully into
// [0,1] BEFORE integer conversion (never convert negative/NaN/huge floats to
// unsigned). Degenerate (edge) and no-intersection ranges yield an invalid
// tile so the caller keeps the original artwork.
inline TileRect promptTileRect(float u0, float v0, float u1, float v1, uint32_t w, uint32_t h) {
    TileRect tile;
    if (!w || !h || w > 2048 || h > 2048) return tile;
    if (!promptFiniteUv(u0) || !promptFiniteUv(v0) || !promptFiniteUv(u1) || !promptFiniteUv(v1))
        return tile;
    float loU = u0 < u1 ? u0 : u1, hiU = u0 < u1 ? u1 : u0;
    float loV = v0 < v1 ? v0 : v1, hiV = v0 < v1 ? v1 : v0;
    if (loU < 0) loU = 0; if (loV < 0) loV = 0;
    if (hiU > 1) hiU = 1; if (hiV > 1) hiV = 1;
    if (!(hiU > loU) || !(hiV > loV)) return tile;
    // All operands now in [0,1] with w,h <= 2048: products are exact in float
    // (< 2^24) and fit uint32_t; no overflow or negative conversion possible.
    const uint32_t x = uint32_t(loU * w), y = uint32_t(loV * h);
    uint32_t x1 = uint32_t(hiU * w + 0.999f), y1 = uint32_t(hiV * h + 0.999f);
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x || y1 <= y || x >= w || y >= h) return tile;
    tile.x = x; tile.y = y; tile.w = x1 - x; tile.h = y1 - y;
    tile.valid = true;
    return tile;
}
// Alpha-weighted mip filtering retains keycap edges without introducing dark
// fringes from transparent texels. Both render paths receive the same chain.
inline void makePromptMips(ColorImage& image) {
    image.authoredMips = true;
    uint32_t width = image.width, height = image.height;
    while (width > 1 || height > 1) {
        const auto& source = image.mips.empty() ? image.pixels : image.mips.back();
        const uint32_t nextWidth = (std::max)(1u, width / 2), nextHeight = (std::max)(1u, height / 2);
        std::vector<uint8_t> pixels(size_t(nextWidth) * nextHeight * 4);
        for (uint32_t y = 0; y < nextHeight; ++y) for (uint32_t x = 0; x < nextWidth; ++x) {
            uint32_t alpha = 0, count = 0, rgb[3]{};
            for (uint32_t dy = 0; dy < 2 && y * 2 + dy < height; ++dy)
                for (uint32_t dx = 0; dx < 2 && x * 2 + dx < width; ++dx) {
                    const auto* input = source.data() + (size_t(y * 2 + dy) * width + x * 2 + dx) * 4;
                    alpha += input[3]; ++count;
                    for (unsigned channel = 0; channel < 3; ++channel) rgb[channel] += input[channel] * input[3];
                }
            auto* output = pixels.data() + (size_t(y) * nextWidth + x) * 4;
            output[3] = uint8_t((alpha + count / 2) / count);
            for (unsigned channel = 0; channel < 3; ++channel)
                output[channel] = alpha ? uint8_t((rgb[channel] + alpha / 2) / alpha) : 0;
        }
        image.mips.push_back(std::move(pixels)); width = nextWidth; height = nextHeight;
    }
}
inline ColorImage composePromptIcon(const ColorImage& base, Button button, Context context, TileRect tile,
                                   const char* configuredLabel = nullptr) {
    ColorImage canvas;
    const uint32_t resolution = base.width <= 1024 && base.height <= 1024 ? XeluLight::kSize / 32 : 1;
    canvas.width = base.width * resolution; canvas.height = base.height * resolution;
    canvas.promptOrigin = base.promptOrigin;
    // Independently reject any tile outside the canvas with overflow-safe
    // bounds before indexing: callers must pass promptTileRect output, but a
    // hand-built tile must never write out of bounds. Invalid canvas (empty
    // pixels) fails ColorImage::valid so resolvers keep the original.
    const uint64_t xEnd = uint64_t(tile.x) + tile.w, yEnd = uint64_t(tile.y) + tile.h;
    if (!tile.valid || !tile.w || !tile.h || !base.width || !base.height ||
        base.width > 2048 || base.height > 2048 ||
        tile.x >= base.width || tile.y >= base.height ||
        xEnd > base.width || yEnd > base.height)
        return canvas;
    tile.x *= resolution; tile.y *= resolution; tile.w *= resolution; tile.h *= resolution;
    canvas.pixels.assign(size_t(canvas.width) * canvas.height * 4, 0);
    const auto icon = iconFor(button, context, configuredLabel);
    if (!icon || !icon->valid()) return canvas;
    float scale = float(tile.w < tile.h ? tile.w : tile.h) / float(icon->width);
    if (!(scale > 0)) return canvas;
    const float dw = icon->width * scale, dh = icon->height * scale;
    const float dx = tile.x + (tile.w - dw) / 2, dy = tile.y + (tile.h - dh) / 2;
    for (uint32_t y = 0; y < tile.h; ++y)
        for (uint32_t x = 0; x < tile.w; ++x) {
            const float sx = (tile.x + x - dx) / scale, sy = (tile.y + y - dy) / scale;
            if (sx < 0 || sy < 0 || sx >= icon->width || sy >= icon->height) continue;
            const uint8_t* in = icon->pixels.data() + (size_t(uint32_t(sy)) * icon->width + uint32_t(sx)) * 4;
            if (!in[3]) continue;
            uint8_t* out = canvas.pixels.data() + (size_t(tile.y + y) * canvas.width + tile.x + x) * 4;
            out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
        }
    makePromptMips(canvas);
    return canvas;
}

// Owned-origin registry: only the reusable icon cache. Classification comes
// from ColorImage::promptOrigin, never from texture ids, pixel samples, or
// raw-pointer keys, so id reuse cannot return stale results. Keys carry canvas
// size and sampled tile so atlas crops never share full-bleed entries. Capped
// at 128 entries (icons rebuild cheaply); steady state is a handful.
// Cache keys store exact button/context/width/height/tile fields and compare
// exactly: hashing may combine, but equality never truncates, so distinct
// sizes or rects cannot return each other's image.
struct IconKey {
    uint8_t button = 0, context = 0;
    uint32_t w = 0, h = 0, tx = 0, ty = 0, tw = 0, th = 0;
    std::string label;
    bool operator==(const IconKey& other) const {
        return button == other.button && context == other.context && w == other.w && h == other.h &&
            tx == other.tx && ty == other.ty && tw == other.tw && th == other.th && label == other.label;
    }
};
struct IconKeyHash {
    size_t operator()(const IconKey& key) const noexcept {
        size_t h = key.button;
        h ^= size_t(key.context) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= size_t(key.w) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= size_t(key.h) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= size_t(key.tx) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= size_t(key.ty) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= size_t(key.tw) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= size_t(key.th) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(key.label) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
struct Registry {
    std::mutex mutex;
    std::unordered_map<IconKey, std::shared_ptr<const ColorImage>, IconKeyHash> icons;
    static Registry& instance() {
        static Registry registry;
        return registry;
    }
    static IconKey iconKey(Button button, Context context, uint32_t w, uint32_t h, TileRect tile) {
        IconKey key;
        key.button = uint8_t(button);
        key.context = uint8_t(context);
        key.w = w; key.h = h;
        key.tx = tile.x; key.ty = tile.y; key.tw = tile.w; key.th = tile.h;
        if (const char* label = keyboardLabel(button, context)) key.label = label;
        return key;
    }
};
inline void clearPromptRegistry() {
    std::lock_guard lock(Registry::instance().mutex);
    Registry::instance().icons.clear();
}
inline Button classifyPromptTexture(uint32_t textureId, const ColorImage& image) {
    (void)textureId;
    return buttonFromOriginByte(image.promptOrigin);
}
// Render-boundary resolver. Returns nullptr to keep the original controller
// artwork: controller source, invalid image, unknown origin, cubemap, or
// resolved render-target generation (faces != 1 handled by caller). UV bounds
// select the sampled tile so atlas crops and wide canvases keep exact sample
// behavior with aspect preserved; pass full-bleed (0,0,1,1) when mesh UVs are
// unavailable (world path).
inline std::shared_ptr<const ColorImage> replacementForUv(uint32_t textureId,
        const std::shared_ptr<const ColorImage>& image, Source source, Context context,
        float u0, float v0, float u1, float v1) {
    (void)textureId;
    if (source != Source::KeyboardMouse || !image || !image->valid() || image->faces != 1) return nullptr;
    const Button button = buttonFromOriginByte(image->promptOrigin);
    if (button == Button::Unknown) return nullptr;
    const TileRect tile = promptTileRect(u0, v0, u1, v1, image->width, image->height);
    if (!tile.valid) return nullptr;
    const bool full = promptFullBleed(u0, v0, u1, v1);
    auto& registry = Registry::instance();
    std::lock_guard lock(registry.mutex);
    const IconKey key = Registry::iconKey(button, context, image->width, image->height, tile);
    if (auto it = registry.icons.find(key); it != registry.icons.end()) return it->second;
    if (registry.icons.size() >= 128) registry.icons.clear();
    auto icon = std::make_shared<const ColorImage>(composePromptIcon(*image, button, context, tile, key.label.c_str()));
    if (!icon || !icon->valid()) return nullptr;
    registry.icons[key] = icon;
    if (!full) {
        static std::atomic<unsigned> tileLogs{0};
        unsigned n = tileLogs++;
        if (n < 8)
            std::fprintf(stderr, "[PromptTile] button=%u size=%ux%u uv=%.3f,%.3f,%.3f,%.3f tile=%u,%u,%ux%u\n",
                unsigned(button), image->width, image->height, u0, v0, u1, v1,
                tile.x, tile.y, tile.w, tile.h);
    }
    return icon;
}
inline std::shared_ptr<const ColorImage> replacementFor(uint32_t textureId,
        const std::shared_ptr<const ColorImage>& image, Source source, Context context) {
    return replacementForUv(textureId, image, source, context, 0, 0, 1, 1);
}
} // namespace DarkRecomp::Prompts
