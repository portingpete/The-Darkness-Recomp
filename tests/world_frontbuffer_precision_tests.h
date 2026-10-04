// Values below one 8-bit step must survive the original ten-bit frontbuffer
// resolve. Read its actual packed storage, before any display calibration.
static void worldFrontbufferPrecisionContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    context->ClearState();WorldRendererD3D11 renderer(device,context,1);
    WorldClear clear;clear.targets[0]=4201;clear.viewport={0,0,8,8};clear.flags=1;
    clear.color={0,0,0,1};renderer.clear(clear);
    for(unsigned x=0;x<8;++x) {
        clear.rectangle=std::array<int32_t,4>{int(x),0,int(x+1),8};
        // Binary-exact FP16 inputs. R10 rounding yields integer codes 1..15;
        // an early RGBA8 conversion would merge these distinct shadow values.
        clear.color={float(x+1)/1024,float(8-x)/1024,float(2*x+1)/1024,1};
        renderer.clear(clear);
    }
    WorldResolve resolve;resolve.targets=clear.targets;resolve.viewport=clear.viewport;
    resolve.rectangle={0,0,8,8};resolve.destination={4202,0x4202000,8,8,54,0};
    require(renderer.resolve(resolve),"Ten-bit frontbuffer resolve rejected");
    const auto frontbuffer=resolve.destination;
    auto read=[&](ID3D11Texture2D* texture) {
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc,nullptr,&staging),"Ten-bit frontbuffer readback allocation");
        context->CopyResource(staging.Get(),texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Ten-bit frontbuffer readback map");
        std::vector<uint32_t> pixels(size_t(desc.Width)*desc.Height);
        for(unsigned y=0;y<desc.Height;++y)
            std::memcpy(pixels.data()+size_t(y)*desc.Width,
                static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(desc.Width)*4);
        context->Unmap(staging.Get(),0);return pixels;
    };
    auto target=[&](unsigned scale,DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=8*scale;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> result;
        check(device->CreateTexture2D(&desc,nullptr,&result),"Ten-bit frontbuffer presentation target");return result;
    };
    unsigned comparisons=0;
    for(unsigned scale:{1u,2u,1u}) {
        renderer.setRenderScale(scale);
        auto packed=target(scale,DXGI_FORMAT_R10G10B10A2_UNORM);
        require(renderer.present(frontbuffer,packed.Get()),"Matching ten-bit frontbuffer presentation rejected");
        const auto codes=read(packed.Get());
        auto compatibility=target(scale,DXGI_FORMAT_R8G8B8A8_UNORM);
        require(renderer.present(frontbuffer,compatibility.Get()),"Ten-bit to eight-bit presentation rejected");
        const auto rgba=read(compatibility.Get());
        for(unsigned y=0;y<8*scale;++y)for(unsigned x=0;x<8*scale;++x) {
            const unsigned logical=x/scale;
            const unsigned r=logical+1,g=8-logical,b=2*logical+1;
            const uint32_t expected=r|(g<<10)|(b<<20)|(3u<<30);
            const auto index=size_t(y)*8*scale+x;
            require(codes[index]==expected,"Frontbuffer resolve or resolution switch lost ten-bit shadow values");
            const auto byte=[](unsigned code){return unsigned(std::floor(double(code)*255/1023+.5));};
            const uint32_t expected8=byte(r)|(byte(g)<<8)|(byte(b)<<16)|(255u<<24);
            require(rgba[index]==expected8,"Ten-bit presentation did not convert once to eight-bit output");
            comparisons+=2;
        }
    }
    std::printf("WorldFrontbufferPrecision: exact R10 shadow codes and R8 conversion, retained1->2->1, %u packed pixel checks.\n",comparisons);
}
