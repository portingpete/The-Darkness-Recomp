#pragma once

static void samplerCachePressure(ID3D11Device* device, ID3D11DeviceContext* context,
                                 const WorldDraw& lightingDraw) {
    context->ClearState();
    WorldRendererD3D11 renderer(device, context);
    auto draw = lightingDraw;
    WorldClear clear;
    clear.targets = draw.targets;
    clear.viewport = draw.viewport;
    clear.flags = 49;
    renderer.clear(clear);
    // More than 512 unique states forces cache eviction during a four-texture
    // material. Every selected state must survive until the draw binds it.
    constexpr unsigned slots[]{0, 1, 2, 4};
    for (unsigned pass = 0; pass < 132; ++pass) {
        for (unsigned index = 0; index < 4; ++index) {
            auto& state = draw.samplers[slots[index]];
            state = {};
            state.valid = true;
            state.address.fill(2);
            state.bias = float(pass * 4 + index) / 32 - 16;
        }
        require(renderer.draw(draw), "Sampler cache pressure draw rejected");
        for (unsigned slot : slots) {
            ComPtr<ID3D11SamplerState> actual;
            context->PSGetSamplers(slot, 1, &actual);
            require(actual != nullptr, "Sampler cache eviction left an unbound material slot");
            D3D11_SAMPLER_DESC desc{};
            actual->GetDesc(&desc);
            if (desc.MipLODBias != draw.samplers[slot].bias) {
                std::fprintf(stderr, "SamplerCache[pass=%u slot=%u] bias=%g expected=%g\n",
                             pass, slot, desc.MipLODBias, draw.samplers[slot].bias);
                throw std::runtime_error("Sampler cache eviction replaced a selected material state");
            }
        }
    }
    context->ClearState();
    puts("Sampler cache pressure preserved all four material states across 528 unique samplers.");
}
