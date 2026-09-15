#pragma once
#include "runtime/native/graphics_settings.h"

// Read actual presented pixels on both hardware and WARP. The owned preview
// image deliberately stays unfiltered so presenting twice cannot compound AA.
static void testPreviewAntialiasing(EnginePreviewD3D11& renderer, ID3D11Device* device,
                                   ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
    const auto saved = graphicsSettings();
    struct Restore { GraphicsSettings settings; ~Restore() { setGraphicsSettings(settings); } } restore{saved};
    auto select = [&](bool enabled) {
        auto selected = saved; selected.antialiasing = enabled; selected.gammaPercent = 100;
        selected.brightnessPercent = 100;
        require(setGraphicsSettings(selected), "Cannot select antialiasing");
    };
    auto white = std::make_shared<AlphaImage>();
    white->width = white->height = 1; white->pixels = {255};
    SimpleMesh triangle; triangle.texture = white;
    triangle.projection = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    triangle.vertices = {
        {{-.875f,.875f,.5f},{0,0},{1,1,1,1}},
        {{.8125f,.5625f,.5f},{1,0},{1,1,1,1}},
        {{-.6875f,-.875f,.5f},{0,1},{1,1,1,1}},
    };
    triangle.indices = {0,1,2};
    auto readOutput = [&] {
        ComPtr<ID3D11Texture2D> back, staging;
        require(SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))), "AA output buffer missing");
        D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "AA readback allocation failed");
        context->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "AA readback map failed");
        std::vector<uint32_t> pixels(size_t(desc.Width) * desc.Height);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(pixels.data() + size_t(y) * desc.Width,
                        static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
        context->Unmap(staging.Get(), 0);
        return pixels;
    };
    // Native size, upscale, pillarbox, letterbox, downscale, then native again.
    for (const auto [width, height] : {std::pair{64u,64u}, {128u,128u}, {128u,64u},
                                      {64u,128u}, {32u,32u}, {64u,64u}}) {
        renderer.releaseDisplayTarget(); context->ClearState();
        require(SUCCEEDED(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)), "AA output resize failed");
        select(false); renderer.render({triangle}); renderer.copyToDisplay();
        const auto original = readOutput();
        const auto sourcePixel = renderer.readPixel(20,20);
        if (width == 64 && height == 64)
            for (const auto pixel : original)
                require(pixel == 0xFF000000 || pixel == 0xFFFFFFFF, "AA Off changed hard rasterized edges");
        select(true); renderer.copyToDisplay();
        const auto filtered = readOutput();
        unsigned changed = 0, intermediate = 0;
        const unsigned extent = std::min(width, height);
        const unsigned left = (width - extent) / 2, top = (height - extent) / 2;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const auto i = size_t(y) * width + x;
            changed += filtered[i] != original[i];
            const auto channel = filtered[i] & 255;
            intermediate += channel > 0 && channel < 255;
            require((filtered[i] >> 24) == 255, "FXAA changed alpha");
            require(((filtered[i] >> 8) & 255) == channel && ((filtered[i] >> 16) & 255) == channel,
                    "FXAA introduced a color tint");
            if (x < left || x >= left + extent || y < top || y >= top + extent)
                require(filtered[i] == 0xFF000000, "FXAA damaged fitted output borders");
        }
        require(changed > 12 && intermediate > 12, "FXAA did not smooth diagonal edges");
        // Flat interiors and the source image stay exact; repeated presentation
        // must not accumulate blur, and Off must recover every original pixel.
        require(filtered.front() == original.front(), "FXAA changed flat background");
        require(renderer.readPixel(20,20) == sourcePixel, "FXAA mutated its source");
        renderer.copyToDisplay();
        require(readOutput() == filtered, "Repeated presentation compounded antialiasing");
        select(false); renderer.copyToDisplay();
        require(readOutput() == original, "Turning FXAA off did not restore original output");
        // A following draw must recover state after the fullscreen AA pass.
        select(true); renderer.render({triangle}); renderer.copyToDisplay();
        require(readOutput() == filtered, "FXAA leaked pipeline state into the next frame");
        for (bool aa : {false,true}) for (unsigned gamma : {50u,80u,100u,150u})
            for (unsigned brightness : {50u,100u,125u,200u}) {
            auto selected=graphicsSettings();selected.antialiasing=aa;selected.gammaPercent=gamma;
            selected.brightnessPercent=brightness;
            setGraphicsSettings(selected);renderer.copyToDisplay();
            const auto corrected=readOutput();const auto& neutral=aa?filtered:original;
            for(size_t i=0;i<corrected.size();++i) {
                for(unsigned c=0;c<3;++c) {
                    // The neutral reference is already quantized to UNORM8;
                    // tone adjustments operate before that rounding. Bound the original
                    // half-code interval, especially at near-black FXAA edges.
                    const double value=(neutral[i]>>(c*8))&255;
                    const double low=255*std::min(1.0,std::pow(std::max(0.0,value-.51)/255,100.0/gamma)*brightness/100.0);
                    const double high=255*std::min(1.0,std::pow(std::min(255.0,value+.51)/255,100.0/gamma)*brightness/100.0);
                    const double actual=(corrected[i]>>(c*8))&255;
                    require(actual>=low-1 && actual<=high+1,
                            "Brightness/gamma differs from final-output curve with AA/resize/borders");
                }
                require((corrected[i]>>24)==(neutral[i]>>24),"Brightness/gamma changed alpha");
            }
            require(renderer.readPixel(20,20)==sourcePixel,"Brightness/gamma changed the owned scene");
            renderer.copyToDisplay();require(readOutput()==corrected,"Repeated brightness/gamma correction accumulated");
        }
        select(false);renderer.copyToDisplay();
        require(readOutput()==original,"Neutral brightness/gamma did not restore the exact image");
    }
    // A flat, non-gray image exercises the early exit and channel preservation.
    SimpleMesh flat = triangle;
    flat.vertices = {{{-1,-1,.5f},{0,0},{.25f,.5f,.75f,1}}, {{3,-1,.5f},{0,0},{.25f,.5f,.75f,1}},
                     {{-1,3,.5f},{0,0},{.25f,.5f,.75f,1}}};
    select(false); renderer.render({flat}); renderer.copyToDisplay();
    const auto flatOriginal = readOutput();
    select(true); renderer.copyToDisplay();
    require(readOutput() == flatOriginal, "FXAA changed a uniform color");
    for(bool aa:{false,true})for(unsigned gamma:{50u,95u,100u,150u})for(unsigned brightness:{50u,100u,125u,200u}) {
        auto selected=graphicsSettings();selected.antialiasing=aa;selected.gammaPercent=gamma;
        selected.brightnessPercent=brightness;
        setGraphicsSettings(selected);renderer.copyToDisplay();
        const auto corrected=readOutput();
        for(size_t i=0;i<corrected.size();++i)for(unsigned c=0;c<3;++c) {
            const double expected=255*std::min(1.0,std::pow(double((flatOriginal[i]>>(c*8))&255)/255,100.0/gamma)*brightness/100.0);
            require(std::abs(double((corrected[i]>>(c*8))&255)-expected)<=1,"Flat-color brightness/gamma or FXAA early exit is wrong");
        }
    }
    select(false);renderer.copyToDisplay();
    require(readOutput()==flatOriginal,"Neutral brightness/gamma did not restore flat colors");
    std::puts("PreviewAntialiasing passed: FXAA, brightness/gamma combinations, neutral restoration, flat colors, resize, borders and repeated frames.");
}
