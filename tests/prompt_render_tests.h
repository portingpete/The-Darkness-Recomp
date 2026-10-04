// Unlike the artwork preview, these draws retain the original controller
// image and let EnginePreview perform replacement/context/size selection.
#include "runtime/native/input.h"
static void testPromptRenderIntegration(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
    using namespace DarkRecomp::Prompts;
    const auto labels = bindingLabelStore().load();
    setBindingLabels(defaultBindingLabels(), defaultGameplayBindingLabels());
    nativeInput().setSettingsOpen(true); nativeInput().setSettingsOpen(false);
    for (unsigned scale : {1u, 2u, 3u}) {
        EnginePreviewD3D11 preview(device, context, swapChain, 64 * scale, 64 * scale, scale);
        auto original = std::make_shared<ColorImage>(promptTestImage(200, 40, 40, 32, 32, uint8_t(Origin::B)));
        SimpleMesh draw;
        draw.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        draw.vertices = {
            {{-.5f, -.5f, .5f}, {0, 1}, {1, 1, 1, 1}}, {{.5f, -.5f, .5f}, {1, 1}, {1, 1, 1, 1}},
            {{.5f, .5f, .5f}, {1, 0}, {1, 1, 1, 1}}, {{-.5f, .5f, .5f}, {0, 0}, {1, 1, 1, 1}},
        };
        draw.indices = {0, 1, 2, 0, 2, 3}; draw.colorTexture = original; draw.textureId = 91;
        draw.promptContext = PromptRenderContext::Gameplay;
        const auto snapshot = draw;
        const auto pixels = original->pixels;
        auto background = draw; background.colorTexture = std::make_shared<ColorImage>(promptTestImage(26, 51, 204));
        background.opaque = true;
        for (auto& vertex : background.vertices) {vertex.position[0] *= 2; vertex.position[1] *= 2;}
        const auto read = [&](unsigned x, unsigned y) {
            return preview.readPixel(x * scale + scale / 2, y * scale + scale / 2);
        };
        const auto unchangedOutside = [&] {
            for (const auto& point : {std::array<unsigned, 2>{11, 32}, {52, 32}, {32, 11}, {32, 52}}) {
                const auto pixel = read(point[0], point[1]);
                nearByte(pixel, 0, 26); nearByte(pixel, 1, 51); nearByte(pixel, 2, 204);
            }
        };
        preview.render({background, draw});
        const auto gameplayOutline = read(15, 32), gameplayFace = read(18, 32), gameplayGlyph = read(29, 28);
        require(((gameplayOutline >> 16) & 255) < 128 && ((gameplayOutline >> 8) & 255) < 128,
                "Simple gameplay prompt did not grow outside its original rectangle");
        for (unsigned channel = 0; channel < 3; ++channel) {
            require(((gameplayFace >> (channel * 8)) & 255) > 165, "Simple gameplay prompt did not select authored R face");
            require(((gameplayGlyph >> (channel * 8)) & 255) < 153, "Simple gameplay R glyph was absent");
        }
        unchangedOutside();
        auto menu = draw; menu.promptContext = PromptRenderContext::Menu;
        preview.render({background, menu});
        const auto menuOutline = read(15, 32);
        nearByte(menuOutline, 0, 26); nearByte(menuOutline, 1, 51); nearByte(menuOutline, 2, 204);
        unchangedOutside();
        auto unknown = draw; unknown.colorTexture = std::make_shared<ColorImage>(promptTestImage(200, 40, 40));
        preview.render({background, unknown});
        nearByte(read(15, 32), 2, 204);
        nearByte(read(32, 32), 0, 200); nearByte(read(32, 32), 1, 40);
        auto triangle = draw; triangle.indices = {0, 1, 2};
        preview.render({background, triangle});
        nearByte(read(15, 32), 2, 204); unchangedOutside();
        preview.render({background, draw});
        require(read(15, 32) == gameplayOutline, "Simple prompt context switch retained stale artwork or geometry");
        require(draw.projection == snapshot.projection && draw.indices == snapshot.indices &&
                draw.vertices.size() == snapshot.vertices.size() &&
                !std::memcmp(draw.vertices.data(), snapshot.vertices.data(), draw.vertices.size() * sizeof(SimpleVertex)) &&
                original->pixels == pixels && original->width == 32,
                "Simple prompt rendering rewrote original geometry, UVs, color, projection or pixels");
        std::printf("SimplePromptRender%u passed: owned gameplay/menu selection, 32->40px, unknown/triangle guards and immutable snapshots.\n", scale);
    }
    setBindingLabels(labels->menu, labels->gameplay);
    context->ClearState();
}
