#pragma once
#include "runtime/native/graphics_settings.h"

static void worldAnisotropyContract(ID3D11Device* device, ID3D11DeviceContext* context,
                                    const WorldDraw& lightingDraw) {
    const auto saved = graphicsSettings();
    struct Restore { GraphicsSettings value; ~Restore() { setGraphicsSettings(value); } } restore{saved};
    context->ClearState();
    WorldRendererD3D11 renderer(device, context);
    auto draw = lightingDraw;
    auto image = std::make_shared<ColorImage>();
    image->width = image->height = 8; image->authoredMips = true; image->firstMip = 1;
    image->mips = {std::vector<uint8_t>(4*4*4, 255), std::vector<uint8_t>(2*2*4, 255),
                   std::vector<uint8_t>(4, 255)};
    draw.textureObjects[0] = {}; draw.textures[0] = image;
    WorldSampler original;
    original.valid = original.lodValid = true;
    original.minLinear = original.magLinear = original.mipLinear = true;
    original.address = {1, 2, 6}; original.bias = -1.5f; original.maxLevel = 3; original.border = 1;
    draw.samplers[0] = original;
    WorldClear clear; clear.targets = draw.targets; clear.viewport = draw.viewport; clear.flags = 49;
    renderer.clear(clear);
    const auto bound = [&](unsigned slot = 0) {
        ComPtr<ID3D11SamplerState> sampler; context->PSGetSamplers(slot, 1, &sampler);
        require(sampler != nullptr, "Anisotropic filtering left a material sampler unbound");
        D3D11_SAMPLER_DESC desc{}; sampler->GetDesc(&desc); return desc;
    };
    const auto select = [&](unsigned levels) {
        auto settings = saved; settings.anisotropyLevels = levels;
        require(setGraphicsSettings(settings), "Cannot select anisotropic filtering");
    };
    for (unsigned levels : {1u, 2u, 4u, 8u, 16u, 1u}) {
        select(levels);
        require(renderer.draw(draw), "Live anisotropic filtering draw rejected");
        const auto state = bound();
        // D3D normalizes unused MaxAnisotropy to zero for linear/point
        // filters. Its value is meaningful only for anisotropic states.
        if(state.Filter != (levels == 1 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_ANISOTROPIC) ||
           (levels>1 && state.MaxAnisotropy != levels))
            std::fprintf(stderr,"WorldAnisotropy[level=%u] program=%s material=%u filter=%u maxAnisotropy=%u lod=%g,%g\n",
                levels,draw.fragmentName.c_str(),unsigned(draw.material),unsigned(state.Filter),state.MaxAnisotropy,
                state.MinLOD,state.MaxLOD);
        require(state.Filter == (levels == 1 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_ANISOTROPIC) &&
                (levels == 1 || state.MaxAnisotropy == levels), "Live filtering selection did not reach the bound material sampler");
        require(state.MinLOD == 1 && state.MaxLOD == 3 && state.MipLODBias == -1.5f &&
                state.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR && state.AddressV == D3D11_TEXTURE_ADDRESS_CLAMP &&
                state.AddressW == D3D11_TEXTURE_ADDRESS_BORDER && state.BorderColor[0] == 1 &&
                state.ComparisonFunc == D3D11_COMPARISON_NEVER,
                "Anisotropic filtering changed residency, addressing, comparison, border or bias");
        require(draw.samplers[0].anisotropy == 1 && draw.samplers[0].minLevel == 0,
                "Enhanced filtering mutated the immutable captured guest sampler");
    }
    // Real gameplay's mipmapped surface textures request 16x. Original must
    // preserve that preference, while every explicit choice must reach D3D.
    draw.samplers[0].anisotropy = 16;
    for (unsigned levels : {1u, 2u, 4u, 8u, 16u, 1u}) {
        select(levels); require(renderer.draw(draw), "Original anisotropic sampler draw rejected");
        const auto state = bound();
        require(state.Filter == D3D11_FILTER_ANISOTROPIC &&
                state.MaxAnisotropy == (levels == 1 ? 16u : levels),
                "Explicit filtering choice was clamped to the game's authored 16x");
        require(state.MinLOD == 1 && state.MaxLOD == 3 && state.MipLODBias == -1.5f &&
                state.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR && state.AddressV == D3D11_TEXTURE_ADDRESS_CLAMP &&
                state.AddressW == D3D11_TEXTURE_ADDRESS_BORDER && state.BorderColor[0] == 1 &&
                state.ComparisonFunc == D3D11_COMPARISON_NEVER && draw.samplers[0].anisotropy == 16,
                "Explicit filtering changed authored LODs, addressing or the captured sampler");
    }
    select(16); draw.samplers[0] = original;
    for (unsigned mode = 0; mode < 3; ++mode) {
        draw.samplers[0] = original;
        if (mode == 0) draw.samplers[0].minLinear = draw.samplers[0].magLinear = false;
        if (mode == 1) draw.samplers[0].magLinear = false;
        if (mode == 2) draw.samplers[0].baseOnly = true;
        require(renderer.draw(draw), "Protected filtering mode draw rejected");
        const auto state = bound();
        require(state.Filter != D3D11_FILTER_ANISOTROPIC &&
                state.MinLOD == 1 && state.MaxLOD == (mode == 2 ? 1 : 3),
                "16x filtering altered point, mixed or base-only texture sampling");
    }
    draw.samplers[0] = original;
    draw.samplers[4] = original; draw.samplers[4].maxLevel = 0; draw.samplers[4].bias = 0;
    require(renderer.draw(draw), "Cube filtering fixture rejected");
    require(bound(4).Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            "16x surface filtering changed cubemap sampling");
    // LF/NDSEATP use the same submission bucket as postprocess programs.
    // Check the actual shader distinction rather than assuming that bucket is sufficient.
    draw.material = WorldMaterial::post; draw.fragmentName = "XRShader_FP20_NDSEATP";
    draw.fragmentFlags = 0; draw.textures[4].reset();
    require(renderer.draw(draw) && bound().MaxAnisotropy == 16,
            "Geometric NDSEATP submission did not receive surface filtering");
    draw.fragmentName = "GUIFadeToWhite";
    draw.fragmentConstants[0] = {0, 0, std::bit_cast<float>(0xFFFFFFFFu), std::bit_cast<float>(0xFFFFFFFFu)};
    draw.fragmentConstants[1] = {};
    require(renderer.draw(draw) && bound().Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            "16x surface filtering changed GUI/postprocess sampling");
    // Video remains protected by its program even if a captured sampler has
    // ordinary linear mip filtering rather than the usual base-only mode.
    draw.fragmentName = "CMWnd_ModTexture_PaintVideo_YUV2RGB";
    draw.samplers[1] = original; draw.samplers[1].maxLevel = 0;
    require(renderer.draw(draw) && bound().Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR &&
            bound(1).Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            "16x surface filtering changed video luma or chroma sampling");
    // Resolve-backed resources include screen data and shadow comparisons.
    // Even a linear sampler in a world material must retain its original filter.
    draw = lightingDraw; draw.samplers[0] = original; draw.samplers[0].bias = 0;
    WorldResolve resolve; resolve.targets = clear.targets; resolve.viewport = clear.viewport;
    resolve.rectangle = clear.viewport; resolve.destination = {971, 4096, 64, 64, 6};
    require(renderer.resolve(resolve), "Resolved filtering safety fixture rejected");
    draw.textureObjects[0] = resolve.destination; draw.textures[0].reset();
    require(renderer.draw(draw) && bound().Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            "16x surface filtering changed resolved or comparison texture sampling");
    context->ClearState();
    std::puts("World anisotropy: live Original/2x/4x/8x/16x, original preference, residency and protected texture modes passed.");
}
