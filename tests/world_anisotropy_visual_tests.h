#pragma once

// A sampler descriptor alone cannot prove AF: constant UVs or solid textures
// render identically with every filter. Exercise the shipped surface shader
// with a 16:1 footprint, a directional pattern and independently authored mips.
static void worldAnisotropyVisualContract(ID3D11Device* device, ID3D11DeviceContext* context) {
    const auto saved = graphicsSettings();
    struct Restore {
        GraphicsSettings settings;
        ID3D11DeviceContext* context;
        ~Restore() { setGraphicsSettings(settings); context->ClearState(); }
    } restore{saved, context};
    context->ClearState();
    WorldRendererD3D11 renderer(device, context);
    constexpr unsigned side = 128, textureSide = 256;
    constexpr uint32_t target = 3911;

    auto image = std::make_shared<ColorImage>();
    image->width = image->height = textureSide;
    image->authoredMips = true;
    image->pixels.resize(textureSide * textureSide * 4);
    for (unsigned y = 0; y < textureSide; ++y) for (unsigned x = 0; x < textureSide; ++x) {
        auto* pixel = image->pixels.data() + (size_t(y) * textureSide + x) * 4;
        pixel[0] = pixel[1] = pixel[2] = (x / 8) & 1 ? 255 : 0;
        pixel[3] = 255;
    }
    // Each mip is a 2x2 area average, not a convenient solid-color LOD fixture.
    // At mip4 the 8-texel stripes average to gray; their narrow-axis detail is
    // still resolvable when the elongated footprint is sampled anisotropically.
    for (unsigned width = textureSide; width > 1; width >>= 1) {
        const auto& previous = image->mips.empty() ? image->pixels : image->mips.back();
        std::vector<uint8_t> mip(size_t(width / 2) * (width / 2) * 4);
        for (unsigned y = 0; y < width / 2; ++y) for (unsigned x = 0; x < width / 2; ++x)
            for (unsigned c = 0; c < 4; ++c) {
                unsigned sum = 0;
                for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx)
                    sum += previous[(size_t(y * 2 + dy) * width + x * 2 + dx) * 4 + c];
                mip[(size_t(y) * (width / 2) + x) * 4 + c] = uint8_t((sum + 2) / 4);
            }
        image->mips.push_back(std::move(mip));
    }
    require(image->valid() && image->mips.size() == 8,
            "AF visual fixture does not have a complete authored mip chain");

    WorldDraw draw;
    draw.targets = {target, 0, 0, 0, 0};
    draw.viewport = {0, 0, side, side};
    draw.material = WorldMaterial::post;
    draw.fragmentName = "XRUtil_RenderSurface";
    draw.textureMask = 1;
    draw.options.modes.fill(4); draw.options.modes[0] = 0;
    for (unsigned lane = 0; lane < 4; ++lane) draw.constants.vectors[lane][lane] = 1;
    draw.constants.references[0][2] = 10; draw.constants.vectors[10] = {1, 1, 1, 1};
    draw.fragmentConstants[0] = {1, 1, 1, 1};
    draw.textures[0] = image;
    auto& sampler = draw.samplers[0];
    sampler.valid = sampler.lodValid = true;
    sampler.minLinear = sampler.magLinear = sampler.mipLinear = true;
    sampler.address.fill(0); sampler.maxLevel = 8;
    put(draw.attributes.data() + 92, 0x01100000); draw.attributes[97] = 8;
    WorldClear clear; clear.targets = draw.targets; clear.viewport = draw.viewport; clear.flags = 1;

    const auto geometry = [](float vExtent) {
        auto result = std::make_shared<StoredGeometry>();
        result->vertexCount = 4; result->stride = 32;
        result->formats[0] = result->formats[1] = 4;
        result->vertices.resize(4 * result->stride); result->indices = {0, 1, 2, 0, 2, 3};
        const EngineVector positions[]{{-1, -1, .5f, 1}, {-1, 1, .5f, 1},
                                       {1, 1, .5f, 1}, {1, -1, .5f, 1}};
        const EngineVector uvs[]{{0, vExtent, 0, 1}, {0, 0, 0, 1},
                                 {.5f, 0, 0, 1}, {.5f, vExtent, 0, 1}};
        for (unsigned vertex = 0; vertex < 4; ++vertex) for (unsigned lane = 0; lane < 4; ++lane) {
            put(result->vertices.data() + vertex * 32 + lane * 4,
                std::bit_cast<uint32_t>(positions[vertex][lane]));
            put(result->vertices.data() + vertex * 32 + 16 + lane * 4,
                std::bit_cast<uint32_t>(uvs[vertex][lane]));
        }
        return result;
    };
    const auto halfFloat = [](uint16_t h) {
        const unsigned exponent = (h >> 10) & 31, mantissa = h & 1023;
        return std::ldexp(double(exponent ? 1024 + mantissa : mantissa),
                          int(exponent ? exponent : 1) - 25) * (h & 0x8000 ? -1 : 1);
    };
    const auto render = [&](unsigned levels, float vExtent) {
        auto settings = saved; settings.anisotropyLevels = levels;
        require(setGraphicsSettings(settings), "Cannot select visual AF fixture filtering");
        auto vertices = geometry(vExtent); draw.geometry = {vertices, vertices, 0, 6};
        renderer.clear(clear);
        require(renderer.draw(draw), "Original surface shader rejected visual AF fixture");
        const auto pixels = renderer.readSurface(target, false);
        require(pixels.size() == size_t(side) * side * 8, "Visual AF readback extent differs");
        std::vector<double> values;
        values.reserve((side - 16) * (side - 16));
        // Exclude raster edges; require covered, finite neutral pixels so an
        // empty target or a changed shader/color state cannot pass as blur.
        for (unsigned y = 8; y < side - 8; ++y) for (unsigned x = 8; x < side - 8; ++x) {
            uint16_t rgba[4]{};
            std::memcpy(rgba, pixels.data() + (size_t(y) * side + x) * 8, sizeof(rgba));
            const double value = halfFloat(rgba[0]);
            require(std::isfinite(value) && value >= 0 && value <= 1.01 &&
                    rgba[0] == rgba[1] && rgba[1] == rgba[2] && std::abs(halfFloat(rgba[3]) - 1) < .002,
                    "Visual AF fixture lost covered grayscale pixels");
            values.push_back(value);
        }
        return values;
    };
    const auto contrast = [](const std::vector<double>& values) {
        double sum = 0;
        for (const double value : values) sum += std::abs(value - .5);
        return sum / values.size();
    };
    const auto difference = [](const std::vector<double>& a, const std::vector<double>& b) {
        require(a.size() == b.size(), "Visual AF comparison extents differ");
        double sum = 0;
        for (size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
        return sum / a.size();
    };

    const auto original = render(1, 8), enhanced = render(16, 8), restored = render(1, 8);
    const double originalContrast = contrast(original), enhancedContrast = contrast(enhanced);
    const double changed = difference(original, enhanced), restoredDifference = difference(original, restored);
    const auto frontOriginal = render(1, .5f), frontEnhanced = render(16, .5f);
    const double frontDifference = difference(frontOriginal, frontEnhanced);
    std::printf("World AF pixels: 16:1 contrast Original=%g 16x=%g delta=%g restored=%g; front-on delta=%g\n",
                originalContrast, enhancedContrast, changed, restoredDifference, frontDifference);
    // Broad bounds tolerate device kernel/LOD precision while rejecting a
    // settings-only implementation, an isotropic footprint or stale bindings.
    require(originalContrast < .03 && enhancedContrast > .20 && changed > .18,
            "16x AF did not preserve directional texture detail over trilinear filtering");
    require(restoredDifference < .002, "Original filtering did not restore after live 16x selection");
    require(contrast(frontOriginal) > .30 && contrast(frontEnhanced) > .30 && frontDifference < .03,
            "AF changed the isotropic front-on control or the pattern was not resolvable");

    // Retail surface samplers can already request 16x. An explicit menu 2x
    // must lower that value; Original must restore the authored 16x instead.
    // This catches max(authored, selected), which makes every menu choice look
    // identical on real authored-16x materials despite the authored-1x case.
    draw.samplers[0].anisotropy = 16;
    const auto selectedTwo = render(2, 8), selectedSixteen = render(16, 8), restoredTwo = render(2, 8);
    const auto authoredOriginal = render(1, 8);
    const double twoContrast = contrast(selectedTwo), sixteenContrast = contrast(selectedSixteen);
    const double selectedDifference = difference(selectedTwo, selectedSixteen);
    std::printf("World AF authored16 pixels: selected2x contrast=%g selected16x=%g delta=%g restored2x=%g Original=%g\n",
                twoContrast, sixteenContrast, selectedDifference, difference(selectedTwo, restoredTwo),
                difference(authoredOriginal, selectedSixteen));
    require(sixteenContrast > twoContrast + .08 && selectedDifference > .08,
            "Explicit 2x AF did not change the pixels of an authored 16x material");
    require(difference(selectedTwo, restoredTwo) < .002 && difference(authoredOriginal, selectedSixteen) < .002,
            "Live AF selection did not restore 2x or Original authored 16x pixels");
}
