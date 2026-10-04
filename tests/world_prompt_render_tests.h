// Exercise selection at the real draw boundary, retaining original owned
// controller pixels and immutable geometry throughout source/context switches.
static void worldPromptRenderPass(WorldRendererD3D11& renderer, unsigned scale) {
    using namespace DarkRecomp::Prompts;
    constexpr uint32_t colorTarget = 5451, depthTarget = 5452;
    auto geometry = std::make_shared<StoredGeometry>();
    geometry->vertexCount = 4; geometry->stride = 32;
    geometry->formats[0] = geometry->formats[1] = 4;
    geometry->vertices.resize(4 * 32); geometry->indices = {0, 1, 2, 0, 2, 3};
    const EngineVector positions[]{{-.5f, -.5f, .5f, 1}, {-.5f, .5f, .5f, 1},
                                   {.5f, .5f, .5f, 1}, {.5f, -.5f, .5f, 1}};
    const EngineVector coordinates[]{{0, 1, 0, 1}, {0, 0, 0, 1},
                                     {1, 0, 0, 1}, {1, 1, 0, 1}};
    for (unsigned vertex = 0; vertex < 4; ++vertex) for (unsigned lane = 0; lane < 4; ++lane) {
        put(geometry->vertices.data() + vertex * 32 + lane * 4, std::bit_cast<uint32_t>(positions[vertex][lane]));
        put(geometry->vertices.data() + vertex * 32 + 16 + lane * 4, std::bit_cast<uint32_t>(coordinates[vertex][lane]));
    }
    auto original = std::make_shared<ColorImage>();
    original->width = original->height = 32; original->promptOrigin = uint8_t(Origin::B);
    original->pixels.resize(32 * 32 * 4);
    for (size_t i = 0; i < original->pixels.size(); i += 4) {
        original->pixels[i] = 200; original->pixels[i + 1] = original->pixels[i + 2] = 40;
        original->pixels[i + 3] = 255;
    }
    WorldDraw draw; draw.geometry = {geometry, geometry, 0, 6};
    draw.viewport = {0, 0, 64, 64}; draw.targets = {colorTarget, 0, 0, 0, depthTarget};
    draw.material = WorldMaterial::fixed; draw.fragmentName = "MRenderXenon_Attrib_TexEnvMode01";
    draw.options.modes.fill(4); draw.options.modes[0] = 0;
    for (unsigned lane = 0; lane < 4; ++lane) draw.constants.vectors[lane][lane] = 1;
    draw.constants.references[0][2] = 10; draw.constants.vectors[10] = {1, 1, 1, 1};
    put(draw.attributes.data() + 92, 0x01100008); draw.attributes[96] = draw.attributes[97] = 8;
    draw.attributes[144] = 5; draw.attributes[145] = 6;
    draw.textureIds[0] = 91; draw.textures[0] = original;
    draw.promptContext = PromptRenderContext::Gameplay;
    const auto originalDraw = draw;
    const auto originalGeometry = *geometry;
    const auto originalPixels = original->pixels;
    const auto labels = bindingLabelStore().load();
    setBindingLabels(defaultBindingLabels(), defaultGameplayBindingLabels());

    WorldClear clear; clear.targets = draw.targets; clear.viewport = draw.viewport;
    clear.flags = 49; clear.color = {.1f, .2f, .8f, 1}; clear.depth = .25f; clear.stencil = 9;
    const unsigned side = 64 * scale;
    const auto halfFloat = [](uint16_t h) {
        const unsigned e = (h >> 10) & 31, m = h & 1023;
        return std::ldexp(double(e ? 1024 + m : m), int(e ? e : 1) - 25) * (h & 0x8000 ? -1 : 1);
    };
    const auto pixel = [&](const std::vector<uint8_t>& image, unsigned x, unsigned y) {
        std::array<double, 4> result{}; uint16_t channels[4]{};
        const auto offset = (size_t(y * scale + scale / 2) * side + x * scale + scale / 2) * 8;
        std::memcpy(channels, image.data() + offset, 8);
        for (unsigned channel = 0; channel < 4; ++channel) result[channel] = halfFloat(channels[channel]);
        return result;
    };
    const auto samePixel = [&](const auto& image, const auto& background, unsigned x, unsigned y) {
        const auto actual = pixel(image, x, y), expected = pixel(background, x, y);
        for (unsigned channel = 0; channel < 4; ++channel)
            require(std::abs(actual[channel] - expected[channel]) < .002, "Prompt boundary pixel changed unexpectedly");
    };
    const auto unchangedOutside = [&](const auto& image, const auto& background, unsigned low, unsigned high) {
        require(image.size() == size_t(side) * side * 8, "Prompt GPU output extent differs from render scale");
        for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x)
            if (x < low * scale || x >= high * scale || y < low * scale || y >= high * scale)
                require(!std::memcmp(image.data() + (size_t(y) * side + x) * 8,
                                     background.data() + (size_t(y) * side + x) * 8, 8),
                        "Prompt changed pixels outside its expected rectangle");
    };
    const auto render = [&](const WorldDraw& requested) {
        renderer.clear(clear); require(renderer.draw(requested), "Owned prompt GPU draw rejected");
        return renderer.readSurface(colorTarget, false);
    };
    renderer.clear(clear); const auto background = renderer.readSurface(colorTarget, false);
    const auto depth = renderer.readSurface(depthTarget, true);
    renderer.setPromptSource(true);
    const auto gameplay = render(draw);
    const auto outline = pixel(gameplay, 15, 32), face = pixel(gameplay, 18, 32);
    require(outline[2] < .5 && outline[1] < .5, "Gameplay keycap did not grow outside its original 32px rectangle");
    require(face[0] > .65 && face[1] > .65 && face[2] > .65, "Owned gameplay B prompt did not select authored R artwork");
    const auto glyph = pixel(gameplay, 29, 28);
    require(glyph[0] < .6 && glyph[1] < .6 && glyph[2] < .6, "Gameplay R glyph is absent from GPU output");
    unchangedOutside(gameplay, background, 12, 52);
    require(renderer.readSurface(depthTarget, true) == depth, "Prompt enlargement modified depth/stencil");

    auto menu = draw; menu.promptContext = PromptRenderContext::Menu;
    const auto menuPixels = render(menu);
    samePixel(menuPixels, background, 15, 32);
    require(menuPixels != gameplay, "Menu and gameplay B prompts reused the same artwork");
    unchangedOutside(menuPixels, background, 12, 52);
    renderer.setPromptSource(false);
    const auto controller = render(draw);
    unchangedOutside(controller, background, 16, 48);
    const auto controllerCenter = pixel(controller, 32, 32);
    require(std::abs(controllerCenter[0] - 200. / 255) < .002 &&
            std::abs(controllerCenter[1] - 40. / 255) < .002,
            "Controller source did not retain original prompt pixels");
    renderer.setPromptSource(true);
    require(render(draw) == gameplay, "Keyboard prompt was not restored after controller source");
    auto unknownImage = std::make_shared<ColorImage>(*original); unknownImage->promptOrigin = 0;
    auto unknown = draw; unknown.textures[0] = unknownImage;
    require(render(unknown) == controller, "Unknown image was replaced or enlarged");
    auto triangle = draw; triangle.geometry.indexCount = 3;
    unchangedOutside(render(triangle), background, 16, 48);
    auto depthDraw = draw; put(depthDraw.attributes.data() + 92, 0x0110000E);
    unchangedOutside(render(depthDraw), background, 16, 48);
    // Draw-boundary adjustments must not rewrite retained original state.
    require(geometry->vertices == originalGeometry.vertices && geometry->indices == originalGeometry.indices &&
            draw.constants.vectors == originalDraw.constants.vectors && draw.attributes == originalDraw.attributes &&
            draw.options == originalDraw.options && draw.viewport == originalDraw.viewport &&
            original->pixels == originalPixels && original->width == 32,
            "Prompt rendering mutated owned geometry, UVs, colors, constants or original pixels");
    setBindingLabels(labels->menu, labels->gameplay);
    std::printf("WorldPromptRender%u passed: owned gameplay/menu, 32->40px, controller restoration, unknown/triangle/depth guards and immutable snapshots.\n", scale);
}
