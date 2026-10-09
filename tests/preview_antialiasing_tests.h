#pragma once
#include "runtime/native/graphics_settings.h"

// Read actual presented pixels on both hardware and WARP. The owned preview
// image deliberately stays unfiltered so presenting twice cannot compound AA.
static void testPreviewAntialiasing(EnginePreviewD3D11& renderer, ID3D11Device* device,
                                   ID3D11DeviceContext* context, IDXGISwapChain* swapChain) {
    const auto saved = graphicsSettings();
    struct Restore { GraphicsSettings settings; ~Restore() { setGraphicsSettings(settings); } } restore{saved};
    auto select = [&](AntialiasingMode mode) {
        auto selected = saved; selected.antialiasing = mode;
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
    std::vector<uint32_t> fxaaNative;
    for(auto filterMode : {AntialiasingMode::FXAA,AntialiasingMode::SMAA}) {
    // Native size, upscale, pillarbox, letterbox, downscale, then native again.
    for (const auto [width, height] : {std::pair{64u,64u}, {128u,128u}, {128u,64u},
                                      {64u,128u}, {32u,32u}, {64u,64u}}) {
        renderer.releaseDisplayTarget(); context->ClearState();
        require(SUCCEEDED(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)), "AA output resize failed");
        select(AntialiasingMode::Off); renderer.render({triangle}); renderer.copyToDisplay();
        const auto original = readOutput();
        const auto sourcePixel = renderer.readPixel(20,20);
        if (width == 64 && height == 64)
            for (const auto pixel : original)
                require(pixel == 0xFF000000 || pixel == 0xFFFFFFFF, "AA Off changed hard rasterized edges");
        select(filterMode); renderer.copyToDisplay();
        const auto filtered = readOutput();
        if(width==64 && height==64) {
            if(filterMode==AntialiasingMode::FXAA)fxaaNative=filtered;
            else require(filtered!=fxaaNative,"SMAA returned the FXAA result instead of morphological AA");
        }
        unsigned changed = 0, intermediate = 0;
        const unsigned extent = std::min(width, height);
        const unsigned left = (width - extent) / 2, top = (height - extent) / 2;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const auto i = size_t(y) * width + x;
            changed += filtered[i] != original[i];
            const auto channel = filtered[i] & 255;
            intermediate += channel > 0 && channel < 255;
            require((filtered[i] >> 24) == 255, "AA changed alpha");
            require(((filtered[i] >> 8) & 255) == channel && ((filtered[i] >> 16) & 255) == channel,
                    "AA introduced a color tint");
            if (x < left || x >= left + extent || y < top || y >= top + extent)
                require(filtered[i] == 0xFF000000, "AA damaged fitted output borders");
        }
        require(changed > 12 && intermediate > 12, "AA did not smooth diagonal edges");
        // Flat interiors and the source image stay exact; repeated presentation
        // must not accumulate blur, and Off must recover every original pixel.
        require(filtered.front() == original.front(), "AA changed flat background");
        require(renderer.readPixel(20,20) == sourcePixel, "AA mutated its source");
        renderer.copyToDisplay();
        require(readOutput() == filtered, "Repeated presentation compounded antialiasing");
        select(AntialiasingMode::Off); renderer.copyToDisplay();
        require(readOutput() == original, "Turning AA off did not restore original output");
        // A following draw must recover state after the fullscreen AA pass.
        select(filterMode); renderer.render({triangle}); renderer.copyToDisplay();
        require(readOutput() == filtered, "AA leaked pipeline state into the next frame");
        for (auto aa : {AntialiasingMode::Off,filterMode}) for (unsigned brightness : {50u,100u,125u,200u}) {
            auto selected=graphicsSettings();selected.antialiasing=aa;
            selected.brightnessPercent=brightness;
            setGraphicsSettings(selected);renderer.copyToDisplay();
            const auto corrected=readOutput();const auto& neutral=aa==filterMode?filtered:original;
            for(size_t i=0;i<corrected.size();++i) {
                for(unsigned c=0;c<3;++c) {
                    // The neutral reference is already quantized to UNORM8;
                    // brightness operates before that rounding. Bound the original
                    // half-code interval, especially at near-black AA edges.
                    const double value=(neutral[i]>>(c*8))&255;
                    const double low=std::min(255.0,std::max(0.0,value-.51)*brightness/100.0);
                    const double high=std::min(255.0,std::min(255.0,value+.51)*brightness/100.0);
                    const double actual=(corrected[i]>>(c*8))&255;
                    require(actual>=low-1 && actual<=high+1,
                            "Brightness differs from final-output intensity with AA/resize/borders");
                }
                require((corrected[i]>>24)==(neutral[i]>>24),"Brightness changed alpha");
            }
            require(renderer.readPixel(20,20)==sourcePixel,"Brightness changed the owned scene");
            renderer.copyToDisplay();require(readOutput()==corrected,"Repeated brightness correction accumulated");
        }
        select(AntialiasingMode::Off);renderer.copyToDisplay();
        require(readOutput()==original,"Neutral brightness did not restore the exact image");
    }
    }
    // Source extent changes exercise the SMAA metrics and target allocations;
    // changing only the swap chain would leave the three AA passes unchanged.
    for(unsigned scale:{1u,2u,1u}) {
        renderer.resizeRenderTarget(64*scale,64*scale,scale);
        select(AntialiasingMode::Off);renderer.render({triangle});renderer.copyToDisplay();
        const auto sharp=readOutput();
        const auto source=renderer.readPixel(20*scale,20*scale);
        select(AntialiasingMode::SMAA);renderer.copyToDisplay();
        const auto smooth=readOutput();
        require(smooth!=sharp,"SMAA failed after changing the owned render resolution");
        require(renderer.readPixel(20*scale,20*scale)==source,"SMAA changed the resized owned source");
        renderer.copyToDisplay();require(readOutput()==smooth,"Resized SMAA accumulated blur on repaint");
        select(AntialiasingMode::Off);renderer.copyToDisplay();
        require(readOutput()==sharp,"Off failed to restore the resized source");
    }
    // A flat, non-gray image exercises the early exit and channel preservation.
    SimpleMesh flat = triangle;
    flat.vertices = {{{-1,-1,.5f},{0,0},{.25f,.5f,.75f,1}}, {{3,-1,.5f},{0,0},{.25f,.5f,.75f,1}},
                     {{-1,3,.5f},{0,0},{.25f,.5f,.75f,1}}};
    select(AntialiasingMode::Off); renderer.render({flat}); renderer.copyToDisplay();
    const auto flatOriginal = readOutput();
    for(auto mode:{AntialiasingMode::FXAA,AntialiasingMode::SMAA}) {
        select(mode); renderer.copyToDisplay();
        require(readOutput() == flatOriginal, "AA changed a uniform color");
    }
    for(auto aa:{AntialiasingMode::Off,AntialiasingMode::FXAA,AntialiasingMode::SMAA})for(unsigned brightness:{50u,100u,125u,200u}) {
        auto selected=graphicsSettings();selected.antialiasing=aa;
        selected.brightnessPercent=brightness;
        setGraphicsSettings(selected);renderer.copyToDisplay();
        const auto corrected=readOutput();
        for(size_t i=0;i<corrected.size();++i)for(unsigned c=0;c<3;++c) {
            const double expected=std::min(255.0,double((flatOriginal[i]>>(c*8))&255)*brightness/100.0);
            require(std::abs(double((corrected[i]>>(c*8))&255)-expected)<=1,"Flat-color brightness or AA early exit is wrong");
        }
    }
    select(AntialiasingMode::Off);renderer.copyToDisplay();
    require(readOutput()==flatOriginal,"Neutral brightness did not restore flat colors");
    // High-precision calibrated input must keep the same color through the
    // intermediates. The independent integer LUT oracle also checks that gamma
    // is applied before AA and is not applied again during presentation.
    renderer.releaseDisplayTarget();context->ClearState();
    {
        EnginePreviewD3D11 precise(device,context,swapChain,64,64,1,true);
        auto gamma=std::make_shared<DisplayGamma>();gamma->piecewise=true;
        for(unsigned i=0;i<128;++i)for(unsigned c=0;c<3;++c)
            gamma->entries[i*3+c]={((i*6+c*64)%800)*64,6*64};
        SimpleMesh gammaState;gammaState.displayGamma=gamma;
        select(AntialiasingMode::Off);precise.render({gammaState,flat});precise.copyToDisplay();
        const auto calibrated=readOutput();
        const auto raw=precise.readPixel(20,20);
        for(auto mode:{AntialiasingMode::Off,AntialiasingMode::FXAA,AntialiasingMode::SMAA})
            for(unsigned brightness:{50u,100u,125u,200u}) {
                auto selected=saved;selected.antialiasing=mode;selected.brightnessPercent=brightness;
                require(setGraphicsSettings(selected),"Cannot select high-precision AA settings");
                precise.copyToDisplay();const auto pixels=readOutput();
                for(auto pixel:pixels) {
                    for(unsigned c=0;c<3;++c) {
                        const unsigned q=unsigned(flat.vertices.front().color[c]*1023.0f+.5f);
                        const auto& entry=gamma->entries[(q/8)*3+c];
                        const unsigned code=(entry[0]*8+(q%8)*entry[1]+256)/512;
                        const double expected=std::min(255.0,code*255.0/1023*brightness/100);
                        require(std::abs(double((pixel>>(c*8))&255)-expected)<=1,
                                "AA lost precision or changed calibrated gamma/brightness");
                    }
                    require(pixel>>24==255,"Calibrated AA changed alpha");
                }
                if(brightness==100)require(pixels==calibrated,"AA changed a flat calibrated color");
                require(precise.readPixel(20,20)==raw,"Calibrated AA mutated the raw source");
                precise.copyToDisplay();require(readOutput()==pixels,"Calibrated AA accumulated on repaint");
            }
        precise.releaseDisplayTarget();context->ClearState();
    }
    select(AntialiasingMode::Off);
    std::puts("PreviewAntialiasing passed: FXAA and SMAA, brightness, flat/high-precision gamma colors, source/output resize, borders and repeated frames.");
}
