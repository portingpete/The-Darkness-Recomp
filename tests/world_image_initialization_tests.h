#pragma once

namespace DarkRecomp {
// Narrow test access lets the real GPU cache be filled with tiny fixtures.
struct WorldImageBudgetTestAccess {
    static size_t residentBytes(const WorldRendererD3D11& renderer) {return renderer.imageBytes_;}
    static size_t setBudget(WorldRendererD3D11& renderer,size_t budget) {
        const auto previous=renderer.imageBudget_;renderer.imageBudget_=budget;return previous;
    }
    static bool contains(const WorldRendererD3D11& renderer,const ColorImage* source) {
        return renderer.images_.contains(source);
    }
    static uint64_t used(const WorldRendererD3D11& renderer,const ColorImage* source) {
        return renderer.images_.at(source).used;
    }
    static std::vector<const ColorImage*> residentSources(const WorldRendererD3D11& renderer) {
        std::vector<const ColorImage*> sources;
        for(const auto& [source,image]:renderer.images_)sources.push_back(source);
        return sources;
    }
};
}

static void imagePreloadBudgetContract(WorldRendererD3D11& renderer,ID3D11DeviceContext* context,
    const WorldDraw& seed,const WorldClear& sourceClear) {
    using Access=DarkRecomp::WorldImageBudgetTestAccess;
    auto makeImage=[](std::array<uint8_t,4> color,unsigned side) {
        auto image=std::make_shared<ColorImage>();image->width=image->height=side;
        image->authoredMips=true;
        if(side==2)image->mips.emplace_back(color.begin(),color.end());
        for(unsigned pixel=0;pixel<side*side;++pixel)
            image->pixels.insert(image->pixels.end(),color.begin(),color.end());
        return image;
    };
    auto active=makeImage({255,0,0,255},1),cold=makeImage({0,0,255,255},2),
         demanded=makeImage({0,255,0,255},2);
    auto draw=seed;draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    draw.fragmentFlags=0;draw.textureObjects={};draw.textureIds={};draw.textures={};
    draw.fragmentConstants[0]={0,0,0,0};draw.constants.vectors[10]={1,1,1,1};
    draw.samplers[0]=decodeWorldSampler({2,0,0,0,0,0});
    put(draw.attributes.data()+92,0x01100000);draw.attributes[96]=draw.attributes[97]=8;
    constantTexgen(draw,0,{.5f,.5f,0,1});
    auto clear=sourceClear;clear.flags=1;clear.color={0,0,0,0};
    draw.textures[0]=active;renderer.clear(clear);
    require(renderer.draw(draw),"Preload budget active image draw rejected");
    const auto resident=Access::residentSources(renderer);
    const auto bytes=Access::residentBytes(renderer);
    struct BudgetRestore {
        WorldRendererD3D11& renderer;size_t previous;
        ~BudgetRestore() {Access::setBudget(renderer,previous);}
    } restore{renderer,Access::setBudget(renderer,bytes+cold->bytes())};
    ComPtr<ID3D11ShaderResourceView> before;context->PSGetShaderResources(0,1,&before);
    const auto uploads=renderer.imageUploadCount(),activeUse=Access::used(renderer,active.get());
    require(renderer.preloadImage(cold) && renderer.imageUploadCount()==uploads+1,
            "Exact-fit preload failed to use spare GPU cache capacity");
    require(Access::residentBytes(renderer)==bytes+cold->bytes() && Access::used(renderer,cold.get())==0,
            "Unused preloaded image did not remain cold in a full cache");
    require(!renderer.preloadImage(demanded) && renderer.imageUploadCount()==uploads+1,
            "Over-budget preload uploaded or evicted a resident image");
    for(const auto* source:resident)
        require(Access::contains(renderer,source),"Refused preload evicted an existing GPU image");
    require(renderer.preloadImage(active) && renderer.preloadImage(cold) &&
            renderer.imageUploadCount()==uploads+1 && Access::used(renderer,active.get())==activeUse &&
            Access::used(renderer,cold.get())==0,
            "Full-cache preload hit duplicated upload or refreshed speculative LRU priority");
    ComPtr<ID3D11ShaderResourceView> unchanged;context->PSGetShaderResources(0,1,&unchanged);
    require(unchanged.Get()==before.Get(),"Budgeted preload changed active draw bindings");
    draw.textures[0]=demanded;renderer.clear(clear);
    require(renderer.draw(draw),"Normal draw was blocked by the preload capacity guard");
    require(renderer.imageUploadCount()==uploads+2 && Access::contains(renderer,active.get()) &&
            Access::contains(renderer,demanded.get()) && !Access::contains(renderer,cold.get()),
            "Normal draw did not evict an unused preload before an active texture");
    for(const auto* source:resident)
        require(Access::contains(renderer,source),"Unused preload displaced a previously resident image");
    const auto pixels=renderer.readSurface(1,false);uint16_t center[4]{};
    std::memcpy(center,pixels.data()+(32*64+32)*8,8);
    require(center[0]==0 && center[1]==0x3c00 && center[2]==0,
            "Normal draw cache pressure changed demanded texture pixels");
    draw.textures[0]=active;renderer.clear(clear);
    require(renderer.draw(draw) && renderer.imageUploadCount()==uploads+2,
            "Active GPU texture was reuploaded after speculative cache pressure");
    std::puts("ImagePreloadBudget: exact fit, full-cache hits, refused uploads, unchanged bindings and cold-first normal eviction passed.");
}

// Read every GPU subresource of an authored cube. Distinct mip, face, row and
// column values expose face-major initialization errors and incorrect pitches.
static void authoredCubeInitializationContract(WorldRendererD3D11& renderer,
    ID3D11Device* device,ID3D11DeviceContext* context,const WorldDraw& lighting,
    const WorldClear& clear) {
    auto color=[](unsigned mip,unsigned face,unsigned x,unsigned y,unsigned channel) {
        return uint8_t(17+face*29+mip*13+x*3+y*7+channel*11);
    };
    auto cube=std::make_shared<ColorImage>();cube->width=cube->height=8;cube->faces=6;
    cube->authoredMips=true;cube->mips.resize(3);
    for(unsigned mip=0;mip<4;++mip) {
        const unsigned side=8u>>mip;auto& pixels=mip?cube->mips[mip-1]:cube->pixels;
        pixels.resize(size_t(side)*side*24);
        for(unsigned face=0;face<6;++face)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
            for(unsigned channel=0;channel<4;++channel)
                pixels[((size_t(face)*side+y)*side+x)*4+channel]=color(mip,face,x,y,channel);
    }
    ComPtr<ID3D11ShaderResourceView> before;context->PSGetShaderResources(4,1,&before);
    const auto uploads=renderer.imageUploadCount();
    require(renderer.preloadImage(cube),"Complete authored cube was not preloaded");
    require(renderer.imageUploadCount()==uploads+1,"Cube preload did not initialize exactly one GPU image");
    ComPtr<ID3D11ShaderResourceView> unchanged;context->PSGetShaderResources(4,1,&unchanged);
    require(unchanged.Get()==before.Get(),"Cube preload changed active draw bindings");
    require(renderer.preloadImage(cube) && renderer.imageUploadCount()==uploads+1,
            "Repeated cube preload uploaded the same image again");
    auto draw=lighting;draw.textureObjects[4]={};draw.textures[4]=cube;
    draw.samplers[4]=decodeWorldSampler({2,0,0,0,0,0});
    renderer.clear(clear);constantTexgen(draw,7,{1,0,0,0});
    require(renderer.draw(draw),"Preloaded authored cube draw rejected");
    require(renderer.imageUploadCount()==uploads+1,"Drawing a preloaded cube uploaded it again");
    ComPtr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(4,1,&view);
    require(view!=nullptr,"Preloaded cube view missing");
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};view->GetDesc(&viewDesc);
    require(viewDesc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURECUBE &&
            viewDesc.TextureCube.MostDetailedMip==0 && viewDesc.TextureCube.MipLevels==4,
            "Complete cube view lost authored levels");
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Preloaded cube resource is not Texture2D");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    require(desc.Width==8 && desc.Height==8 && desc.MipLevels==4 && desc.ArraySize==6 &&
            desc.Usage==D3D11_USAGE_IMMUTABLE && desc.BindFlags==D3D11_BIND_SHADER_RESOURCE &&
            desc.MiscFlags==D3D11_RESOURCE_MISC_TEXTURECUBE,"Complete cube initialization changed resource shape");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging),"Cube subresource staging");
    context->CopyResource(staging.Get(),texture.Get());
    unsigned comparisons=0;
    for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<4;++mip) {
        const unsigned side=8u>>mip,index=D3D11CalcSubresource(mip,face,4);
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),index,D3D11_MAP_READ,0,&mapped),"Cube subresource map");
        bool matched=true;
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)for(unsigned channel=0;channel<4;++channel) {
            const auto actual=static_cast<const uint8_t*>(mapped.pData)[size_t(y)*mapped.RowPitch+x*4+channel];
            matched=matched && actual==color(mip,face,x,y,channel);++comparisons;
        }
        context->Unmap(staging.Get(),index);
        require(matched,"Complete authored cube face/mip pixels or row pitch differ");
    }
    std::printf("ImageInitialization: complete authored cube, 24 GPU subresources and %u bytes; preload preserves bindings and shares draw upload.\n",comparisons);
}
