// At 1080p the guest resolves 1290 columns from a surface last drawn to 1288.
// The missing partial tile is black; it must not discard the entire frame.
static void resolvePartialTilePass(WorldRendererD3D11& renderer,ID3D11Device* device,
                                   ID3D11DeviceContext* context,unsigned scale) {
    WorldClear source;source.targets={3301,0,0,0,0};
    source.viewport={0,0,62,32};source.flags=1;source.color={.25f,.5f,.75f,1};
    renderer.clear(source);
    WorldResolve copy;copy.targets=source.targets;copy.flags=0x300;
    copy.viewport={0,0,64,32};copy.rectangle={0,0,64,32};
    copy.destination={3302,0x3302000,64,32,54,0};
    require(renderer.resolve(copy),"Partial-tile frontbuffer resolve rejected");

    D3D11_TEXTURE2D_DESC desc{};desc.Width=64*scale;desc.Height=32*scale;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> output,staging;
    check(device->CreateTexture2D(&desc,nullptr,&output),"Partial-tile output texture");
    require(renderer.present(copy.destination,output.Get()),"Partial-tile frontbuffer presentation rejected");
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc,nullptr,&staging),"Partial-tile readback texture");
    context->CopyResource(staging.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Partial-tile readback");
    const auto* pixels=static_cast<const uint8_t*>(mapped.pData);
    const unsigned y=16*scale;
    for(unsigned x : {0u,61u*scale,62u*scale,64u*scale-1}) {
        const auto* pixel=pixels+size_t(y)*mapped.RowPitch+x*4;
        const bool drawn=x<62*scale;
        require(std::abs(int(pixel[0])-(drawn?64:0))<=1 &&
                std::abs(int(pixel[1])-(drawn?128:0))<=1 &&
                std::abs(int(pixel[2])-(drawn?191:0))<=1,
                "Partial-tile resolve lost drawn color or failed to zero the fringe");
    }
    context->Unmap(staging.Get(),0);
    std::printf("ResolvePartialTile%u: drawn pixels retained and undrawn edge zeroed.\n",scale);
}
