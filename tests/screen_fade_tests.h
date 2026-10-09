#pragma once

// Exercise the original 823471F8 matrix rebuild and 8216FEA8 quad transform
// used by 823F9630's player fade. No guest game loop, saves, desktop input or
// synthetic replacement of an original PPC function is involved.
#include "renderer/d3d11/display_context_d3d11.h"
#include "renderer/d3d11/engine_preview.h"
#include "runtime/native/graphics_settings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

extern "C" PPC_FUNC(__imp__sub_823471F8);

namespace ScreenFadeTestDetail {
using namespace DarkRecomp;
using namespace DarkRecomp::Native;
constexpr uint32_t fadeCaller = 0x823F98F0;
constexpr uint32_t menuCaller = 0x823A8668;

struct Fixture {
    uint32_t block = memory->allocate(4096);
    NativeVideoMode oldMode = nativeVideoMode();
    GraphicsSettings oldSettings = graphicsSettings();
    PPCContext* oldContext = currentContext;
    HWND window = nullptr;
    Fixture() {
        check(block != 0, "Cannot allocate screen fade fixture");
    }
    ~Fixture() {
        if (window) DestroyWindow(window);
        currentContext = oldContext;
        setNativeVideoMode(oldMode.width, oldMode.height);
        setGraphicsSettings(oldSettings);
        memory->release(block);
    }
};

void putFloat(uint32_t address, float value) {
    memory->write32(address, std::bit_cast<uint32_t>(value));
}
float getFloat(uint32_t address) {
    return std::bit_cast<float>(memory->read32(address));
}

std::array<uint32_t,16> rebuild(const PPCContext& initial, uint32_t drawContext,
                               NativeVideoMode mode, uint32_t caller, bool original, bool initialize = true) {
    if (initialize) {
        std::memset(memory->base() + drawContext, 0, 704);
        putFloat(drawContext + 336, float(mode.width) / 640);
        putFloat(drawContext + 340, float(mode.height) / 480);
        // A clean original CView cache, with the same dimensions as the viewport.
        // 8275EEF8 then returns its existing +280/+284 cache without rebuilding.
        putFloat(drawContext + 632, float(mode.width));
        putFloat(drawContext + 636, float(mode.height));
        // 823471F8 subtracts this original depth constant before constructing its
        // scale/translation. A unit offset selects the original unit-depth plane.
        putFloat(drawContext + 656, getFloat(0x82A480C4) + 1);
        memory->write32(drawContext + 676, mode.width);
        memory->write32(drawContext + 680, mode.height);
    }
    PPCContext guest;
    std::memcpy(&guest, &initial, sizeof guest);
    guest.r3.u64 = drawContext;
    guest.lr = caller;
    auto* previousContext = currentContext;
    currentContext = &guest;
    if (original) __imp__sub_823471F8(guest, memory->base());
    else sub_823471F8(guest, memory->base());
    currentContext = previousContext;
    check(guest.r1.u32 == initial.r1.u32 && uint32_t(guest.lr) == caller,
          "Screen fade matrix rebuild changed stack or return address");
    check(getFloat(drawContext + 336) == float(mode.width) / 640 &&
          getFloat(drawContext + 340) == float(mode.height) / 480,
          "Screen fade rebuild changed logical layout scales");
    std::array<uint32_t,16> result{};
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i] = memory->read32(drawContext + 272 + i * 4);
        check(std::isfinite(std::bit_cast<float>(result[i])),
              "Screen fade original matrix produced nonfinite values");
    }
    return result;
}

SimpleMesh transformQuad(const PPCContext& initial, uint32_t block) {
    constexpr float positions[4][3]{{0,0,0},{640,0,0},{640,480,0},{0,480,0}};
    const uint32_t vertices = block + 1024;
    for (unsigned v = 0; v < 4; ++v)
        for (unsigned c = 0; c < 3; ++c) putFloat(vertices + v * 12 + c * 4, positions[v][c]);
    PPCContext guest;
    std::memcpy(&guest, &initial, sizeof guest);
    guest.r3.u64 = guest.r4.u64 = vertices;
    guest.r5.u64 = block + 272;
    guest.r6.u64 = 4;
    auto* previousContext = currentContext;
    currentContext = &guest;
    sub_8216FEA8(guest, memory->base());
    currentContext = previousContext;
    SimpleMesh quad;
    quad.projection = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    quad.indices = {0,1,2,0,2,3};
    quad.vertices.resize(4);
    for (unsigned v = 0; v < 4; ++v) {
        for (unsigned c = 0; c < 3; ++c) quad.vertices[v].position[c] = getFloat(vertices + v * 12 + c * 4);
        for (float& color : quad.vertices[v].color) color = 1;
    }
    auto white = std::make_shared<ColorImage>();
    white->width = white->height = 1;
    white->pixels = {255,255,255,255};
    quad.colorTexture = white;
    return quad;
}

void coverage(DarkRecomp::EnginePreviewD3D11& renderer, DarkRecomp::CDisplayContextD3D11& display,
              const SimpleMesh& fadeQuad, uint32_t width, uint32_t height, unsigned alpha, bool white) {
    auto background = fadeQuad;
    background.opaque = true;
    for (auto& vertex : background.vertices) {
        vertex.color[0] = 64.0f / 255;
        vertex.color[1] = 128.0f / 255;
        vertex.color[2] = 192.0f / 255;
        vertex.color[3] = 1;
    }
    auto fade = fadeQuad;
    for (auto& vertex : fade.vertices) {
        vertex.color[0] = vertex.color[1] = vertex.color[2] = white ? 1.0f : 0.0f;
        vertex.color[3] = float(alpha) / 255;
    }
    renderer.render({background, fade});
    renderer.copyToDisplay();
    ComPtr<ID3D11Texture2D> back, staging;
    check(SUCCEEDED(display.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(&back))), "Screen fade output buffer missing");
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    check(desc.Width == width && desc.Height == height, "Screen fade output dimensions differ");
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(SUCCEEDED(display.GetDevice()->CreateTexture2D(&desc, nullptr, &staging)), "Screen fade staging allocation failed");
    display.GetContext()->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(SUCCEEDED(display.GetContext()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Screen fade readback failed");
    constexpr unsigned backgroundRgb[]{64,128,192};
    bool correct = true;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + x * 4;
            for (unsigned c = 0; c < 3; ++c) {
                const int expected = int(std::lround((backgroundRgb[c] * (255 - alpha) + (white ? 255u : 0u) * alpha) / 255.0));
                correct &= std::abs(int(pixel[c]) - expected) <= 1;
            }
            correct &= pixel[3] == 255;
        }
    display.GetContext()->Unmap(staging.Get(), 0);
    check(correct, "Screen fade left uncovered pixels or changed fade color/alpha");
}
} // namespace ScreenFadeTestDetail

static void testScreenFade(PPCContext& ctx) {
    using namespace ScreenFadeTestDetail;
    Fixture fixture;
    auto settings = fixture.oldSettings;
    settings.antialiasing = AntialiasingMode::Off;
    settings.brightnessPercent = 100;
    check(setGraphicsSettings(settings), "Cannot set neutral screen fade test presentation");
    fixture.window = CreateWindowExW(0, L"STATIC", L"Screen fade contract", WS_OVERLAPPEDWINDOW,
        0, 0, 128, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(fixture.window != nullptr, "Cannot create hidden screen fade test window");
    CDisplayContextD3D11 display;
    display.Init(fixture.window, 1280, 720);
    uint64_t pixelChecks = 0;
    for (const NativeVideoMode mode : std::array<NativeVideoMode,3>{{{1280,720}, {3440,1440}, {2560,720}}}) {
        check(setNativeVideoMode(mode.width, mode.height), "Screen fade video mode rejected");
        display.Resize(mode.width, mode.height);
        EnginePreviewD3D11 renderer(display.GetDevice(), display.GetContext(), display.GetSwapChain(), mode.width, mode.height);
        const auto original = rebuild(ctx, fixture.block, mode, fadeCaller, true);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            check(rebuild(ctx, fixture.block, mode, fadeCaller, false, repeat == 0) == original,
                  "Player fade matrix differs from the original full-screen transform");
        }
        const auto fade = transformQuad(ctx, fixture.block);
        check(std::abs(fade.vertices[0].position[0] + 1) < .00001f &&
              std::abs(fade.vertices[1].position[0] - 1) < .00001f &&
              std::abs(fade.vertices[0].position[1] + 1) < .00001f &&
              std::abs(fade.vertices[2].position[1] - 1) < .00001f,
              "Original player fade quad does not reach every viewport edge");
        for (bool white : {false, true}) for (unsigned alpha : {0u,128u,255u}) {
            coverage(renderer, display, fade, mode.width, mode.height, alpha, white);
            pixelChecks += uint64_t(mode.width) * mode.height;
        }
        for (const uint32_t caller : {menuCaller, fadeCaller - 4, fadeCaller + 4}) {
            rebuild(ctx, fixture.block, mode, caller, false);
            const auto menu = transformQuad(ctx, fixture.block);
            const float halfWidth = (std::min)(1.0f, float(mode.height) * (16.0f / 9) / mode.width);
            check(std::abs(menu.vertices[0].position[0] + halfWidth) < .00001f &&
                  std::abs(menu.vertices[1].position[0] - halfWidth) < .00001f,
                  "Player fade exception changed ordinary menu fitting or adjacent callers");
        }
    }
    std::printf("ScreenFadeContract: original guest matrix/quad transforms, repeated rebuilds, preserved menu/adjacent callers; %llu GPU pixels at 16:9, 21:9 and 32:9 with black/white alpha0/128/255 passed.\n", pixelChecks);
}
