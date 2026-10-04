// The original 324x18 color cube is copied into a scaled render target before
// Final5 samples it. Its black corner must remain black at every render scale;
// bilinear wrapping otherwise mixes in the bright opposite edges at 2x/3x.
static void colorLookupUpscalePass(WorldRendererD3D11& renderer,unsigned scale) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=20;
    geometry->formats[0]=3;geometry->formats[1]=2;
    geometry->vertices.resize(4*20);geometry->indices={0,1,2,0,2,3};
    const float positions[4][3]{{-1,-1,.5f},{-1,1,.5f},{1,1,.5f},{1,-1,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*20;
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned c=0;c<2;++c)put(p+12+c*4,std::bit_cast<uint32_t>(uvs[v][c]));
    }
    auto lookup=std::make_shared<ColorImage>();
    lookup->width=324;lookup->height=18;
    lookup->pixels.assign(size_t(lookup->width)*lookup->height*4,255);
    lookup->pixels[0]=lookup->pixels[1]=lookup->pixels[2]=0;

    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.targets={3201,0,0,0,0};draw.viewport={0,0,324,18};
    draw.material=WorldMaterial::post;draw.fragmentName="XREngine_CCFuser";
    draw.fragmentFlags=0;draw.fragmentConstants[0]={1,1,1,1};
    draw.options.modes.fill(4);draw.options.modes[0]=0;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    draw.textures[1]=lookup;
    auto& sampler=draw.samplers[1];sampler.valid=true;
    sampler.minLinear=sampler.magLinear=sampler.mipLinear=true;
    sampler.address.fill(0); // The captured color-map sampler wraps.
    put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
    renderer.clear(clear);
    require(renderer.draw(draw),"Color-cube copy rejected");
    const auto pixels=renderer.readSurface(3201,false);
    const unsigned width=324*scale;
    require(pixels.size()==size_t(width)*18*scale*8,"Scaled color-cube extent differs");
    auto rgb=[&](unsigned x,unsigned y) {
        std::array<uint16_t,3> channels{};
        std::memcpy(channels.data(),pixels.data()+(size_t(y)*width+x)*8,6);
        return channels;
    };
    for(unsigned y=0;y<scale;++y)for(unsigned x=0;x<scale;++x)
        require(rgb(x,y)==std::array<uint16_t,3>{0,0,0},"Color-cube black corner picked up wrapped neighbor texels");
    require(rgb(scale,0)[0]==0x3c00 && rgb(0,scale)[0]==0x3c00,
            "Color-cube neighboring texels were not copied into scaled blocks");
    std::printf("ColorLookup%u: black corner and adjacent color texels retained.\n",scale);
}

// The retail chain draws a 324x18 quad inside a 1280x720 viewport, resolves
// each CCFuser stage, then samples the resolved cube as data in Final5. Copying
// a texel into a scaled block is insufficient: fractional cube coordinates
// must still interpolate the original neighboring colors with the same weight.
static std::shared_ptr<StoredGeometry> colorGradeQuad(unsigned width,unsigned height,
                                                    unsigned viewportWidth,unsigned viewportHeight) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=20;
    geometry->formats[0]=3;geometry->formats[1]=2;
    geometry->vertices.resize(80);geometry->indices={0,1,2,0,2,3};
    const float right=-1+2.0f*width/viewportWidth,bottom=1-2.0f*height/viewportHeight;
    const float positions[4][3]{{-1,bottom,.5f},{-1,1,.5f},{right,1,.5f},{right,bottom,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*20;
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned c=0;c<2;++c)put(p+12+c*4,std::bit_cast<uint32_t>(uvs[v][c]));
    }
    return geometry;
}
static WorldDraw colorGradeDraw(const std::shared_ptr<StoredGeometry>& geometry,
                                uint32_t target,unsigned width,unsigned height) {
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.targets={target,0,0,0,0};draw.viewport={0,0,width,height};
    draw.material=WorldMaterial::post;
    draw.options.modes.fill(4);draw.options.modes[0]=0;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    return draw;
}
static WorldSampler colorGradeSampler(bool linear,uint8_t address) {
    WorldSampler sampler;sampler.valid=true;sampler.baseOnly=true;
    sampler.minLinear=sampler.magLinear=sampler.mipLinear=linear;
    sampler.address.fill(address);return sampler;
}
static std::shared_ptr<ColorImage> colorGradeCube(bool nonlinear) {
    auto image=std::make_shared<ColorImage>();image->width=324;image->height=18;
    image->authoredMips=true;image->pixels.resize(324*18*4);
    for(unsigned b=0;b<18;++b)for(unsigned g=0;g<18;++g)for(unsigned r=0;r<18;++r) {
        const double red=r/17.0,green=g/17.0,blue=b/17.0;
        const std::array<double,3> color=nonlinear?
            std::array<double,3>{.8*std::pow(red,.55)+.2*green*blue,
                                 .75*std::pow(green,1.4)+.25*red*blue,
                                 .8*std::pow(blue,.8)+.2*red*green}:
            std::array<double,3>{red,green,blue};
        auto* pixel=image->pixels.data()+(size_t(g)*324+b*18+r)*4;
        for(unsigned c=0;c<3;++c)pixel[c]=uint8_t(std::lround(color[c]*255));
        pixel[3]=255;
    }
    return image;
}
static WorldTexture colorGradeResolve(WorldRendererD3D11& renderer,const WorldDraw& draw,
                                      uint32_t object,unsigned width,unsigned height,
                                      uint32_t format=6,int exponent=0) {
    WorldResolve resolve;resolve.targets=draw.targets;resolve.rectangle={0,0,width,height};
    resolve.viewport=draw.viewport;resolve.destination={object,object*4096u,width,height,format,exponent};
    resolve.exponent=-exponent;
    require(renderer.resolve(resolve),"Color-grade resolve rejected");return resolve.destination;
}
static std::vector<uint8_t> colorGradeRead(WorldRendererD3D11& renderer,ID3D11Device* device,
                                          ID3D11DeviceContext* context,const WorldTexture& texture,
                                          unsigned scale) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=texture.width*scale;desc.Height=texture.height*scale;
    const bool wide=texture.format==26;
    require(wide || texture.format==6,"Unsupported color-grade readback format");
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=wide?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> output,staging;
    check(device->CreateTexture2D(&desc,nullptr,&output),"Color-grade output texture");
    require(renderer.present(texture,output.Get()),"Color-grade resolved output presentation rejected");
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc,nullptr,&staging),"Color-grade staging texture");
    context->CopyResource(staging.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Color-grade readback");
    std::vector<uint8_t> pixels(size_t(desc.Width)*desc.Height*4);
    for(unsigned y=0;y<desc.Height;++y) {
        const auto* row=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch;
        if(!wide)std::memcpy(pixels.data()+size_t(y)*desc.Width*4,row,size_t(desc.Width)*4);
        else for(unsigned x=0;x<desc.Width;++x) {
            std::array<uint16_t,4> stored{};std::memcpy(stored.data(),row+size_t(x)*8,8);
            for(unsigned c=0;c<4;++c) {
                const double restored=double(stored[c])/65535*std::exp2(double(texture.exponent));
                pixels[(size_t(y)*desc.Width+x)*4+c]=uint8_t(std::lround(std::clamp(restored,0.0,1.0)*255));
            }
        }
    }
    context->Unmap(staging.Get(),0);return pixels;
}
static bool colorGradeBlocks(const std::vector<uint8_t>& pixels,unsigned scale,
                              const char* name,unsigned stage) {
    unsigned mismatches=0,maximum=0;
    const unsigned physicalWidth=324*scale;
    require(pixels.size()==size_t(physicalWidth)*18*scale*4,"Color-grade LUT extent differs");
    for(unsigned y=0;y<18;++y)for(unsigned x=0;x<324;++x) {
        const auto* first=pixels.data()+(size_t(y*scale)*physicalWidth+x*scale)*4;
        for(unsigned dy=0;dy<scale;++dy)for(unsigned dx=0;dx<scale;++dx)for(unsigned c=0;c<3;++c) {
            const int value=pixels[(size_t(y*scale+dy)*physicalWidth+x*scale+dx)*4+c];
            const unsigned delta=unsigned(std::abs(value-int(first[c])));
            maximum=(std::max)(maximum,delta);mismatches+=delta>1;
        }
    }
    std::printf("ColorGradeBlocks: case=%s stage=%u scale=%u maxDelta=%u mismatches=%u\n",
                name,stage,scale,maximum,mismatches);
    return mismatches==0;
}
// Independent trilinear interpolation over the scale1 resolved RGBA8 cube.
// Final5_4 squares and unsquares the input. Final5_14 additionally applies
// exposure after adding black bloom; model that transfer before the lookup.
static std::array<uint8_t,3> colorGradeOracle(const std::vector<uint8_t>& cube,
                                            const std::array<uint8_t,3>& input,unsigned flags) {
    std::array<unsigned,3> lower{},upper{};
    std::array<double,3> fraction{};
    for(unsigned c=0;c<3;++c) {
        const double source=input[c]/255.0;
        const double linear=flags==14?1-std::exp(-(std::max)(source*source,1e-7)):source*source;
        const double value=std::sqrt((std::max)(linear,1e-8))*17;
        lower[c]=unsigned(std::floor(value));upper[c]=(std::min)(lower[c]+1,17u);
        fraction[c]=value-lower[c];
    }
    std::array<uint8_t,3> result{};
    for(unsigned c=0;c<3;++c) {
        double color=0;
        for(unsigned b=0;b<2;++b)for(unsigned g=0;g<2;++g)for(unsigned r=0;r<2;++r) {
            const unsigned red=r?upper[0]:lower[0],green=g?upper[1]:lower[1],blue=b?upper[2]:lower[2];
            const double weight=(r?fraction[0]:1-fraction[0])*(g?fraction[1]:1-fraction[1])*
                                (b?fraction[2]:1-fraction[2]);
            color+=cube[(size_t(green)*324+blue*18+red)*4+c]*weight;
        }
        result[c]=uint8_t(std::lround(color));
    }
    return result;
}
static void colorGradeChainContract(ID3D11Device* device,ID3D11DeviceContext* context,
                                   uint32_t lutFormat=6,int lutExponent=0) {
    constexpr std::array<const char*,10> names{"copy0","lerp1","append2","gamma4","chain0-2-4","append2-fractional",
        "append2-resolved-clamp","append2-resolved-wrap","append2-resolved-mirror","append2-resolved-border"};
    constexpr std::array<std::array<uint8_t,3>,24> swatches{{
        {0,0,0},{1,1,1},{2,2,2},{3,3,3},{5,5,5},{8,8,8},{12,12,12},{16,16,16},
        {24,24,24},{32,32,32},{48,48,48},{64,64,64},{96,96,96},{128,128,128},
        {192,192,192},{224,224,224},{255,255,255},{3,5,12},{12,3,5},{5,12,3},
        {23,47,79},{101,137,173},{231,17,89},{17,231,89}}};
    constexpr unsigned width=unsigned(swatches.size())*4,height=8;
    const auto identity=colorGradeCube(false),nonlinear=colorGradeCube(true);
    auto scene=std::make_shared<ColorImage>();scene->width=width;scene->height=height;scene->authoredMips=true;
    scene->pixels.resize(size_t(width)*height*4);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        auto* p=scene->pixels.data()+(size_t(y)*width+x)*4;
        std::copy(swatches[x/4].begin(),swatches[x/4].end(),p);p[3]=255;
    }
    auto fractional=std::make_shared<ColorImage>();fractional->width=fractional->height=1;
    fractional->authoredMips=true;fractional->pixels={3,5,12,255};
    auto black=std::make_shared<ColorImage>();black->width=black->height=1;black->pixels={0,0,0,255};
    const auto cubeGeometry=colorGradeQuad(324,18,1280,720);
    const auto sceneGeometry=colorGradeQuad(width,height,width,height);
    std::array<std::vector<uint8_t>,20> baseline;
    bool passed=true;unsigned comparisons=0;
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        for(unsigned kind=0;kind<names.size();++kind) {
            auto fuser=colorGradeDraw(cubeGeometry,3301,1280,720);
            fuser.fragmentName="XREngine_CCFuser";fuser.fragmentConstants[0]={1,1,1,1};
            fuser.samplers[0]=colorGradeSampler(false,2);fuser.samplers[1]=colorGradeSampler(true,0);
            fuser.textures[0]=identity;fuser.textures[1]=nonlinear;
            fuser.fragmentFlags=kind<4?std::array<unsigned,4>{0,1,2,4}[kind]:2;
            if(kind==1)fuser.fragmentConstants[0]={.9f,.8f,.95f,.35f};
            if(kind==3) {
                fuser.fragmentConstants[1]={1.4f,.8f,1.1f,1};fuser.fragmentConstants[2]={0,0,0,0};
            }
            if(kind>=5)fuser.textures[0]=fractional;
            if(kind>=6) {
                // Exercise the append consumer of a resolved color cube with
                // fractional red/green coordinates. All address modes are
                // in range here, so the independent cube oracle applies.
                auto copy=fuser;copy.fragmentFlags=0;copy.textures[1]=nonlinear;
                WorldClear clear;clear.targets=copy.targets;clear.viewport=copy.viewport;clear.flags=1;
                renderer.clear(clear);require(renderer.draw(copy),"Append source color-cube copy rejected");
                const auto resolved=colorGradeResolve(renderer,copy,3700+kind,324,18,lutFormat,lutExponent);
                fuser.textureObjects[1]=resolved;fuser.textures[1].reset();
                fuser.samplers[1]=colorGradeSampler(true,std::array<uint8_t,4>{2,0,1,6}[kind-6]);
            }
            WorldTexture lookup;
            std::vector<uint8_t> cubePixels;
            const unsigned stages=kind==4?3:1;
            for(unsigned stage=0;stage<stages;++stage) {
                if(kind==4) {
                    fuser.fragmentFlags=std::array<unsigned,3>{0,2,4}[stage];
                    if(stage==0)fuser.textures[1]=identity;
                    else {
                        fuser.textureObjects[0]=lookup;fuser.textures[0].reset();
                        fuser.samplers[0]=colorGradeSampler(true,2);fuser.textures[1]=nonlinear;
                    }
                    fuser.fragmentConstants[1]={1,1,1,1};fuser.fragmentConstants[2]={0,0,0,0};
                }
                WorldClear clear;clear.targets=fuser.targets;clear.viewport=fuser.viewport;clear.flags=1;
                renderer.clear(clear);require(renderer.draw(fuser),"Color-grade CCFuser draw rejected");
                lookup=colorGradeResolve(renderer,fuser,3400+kind*4+stage,324,18,lutFormat,lutExponent);
                cubePixels=colorGradeRead(renderer,device,context,lookup,scale);
                passed=colorGradeBlocks(cubePixels,scale,names[kind],fuser.fragmentFlags)&&passed;
            }
            for(unsigned flags:{4u,14u}) {
                const unsigned row=kind*2+(flags==14);
                auto final=colorGradeDraw(sceneGeometry,3302,width,height);
                final.fragmentName="XREngine_Final5";final.fragmentFlags=flags;
                final.textures[0]=scene;final.textures[1]=black;final.textureObjects[2]=lookup;
                final.samplers[0]=final.samplers[1]=colorGradeSampler(false,2);
                final.samplers[2]=colorGradeSampler(true,2);
                final.fragmentConstants[0]={1,1,1,1};final.fragmentConstants[1]={0,0,1,1};
                WorldClear clear;clear.targets=final.targets;clear.viewport=final.viewport;clear.flags=1;
                renderer.clear(clear);require(renderer.draw(final),"Color-grade Final5 draw rejected");
                const auto output=colorGradeResolve(renderer,final,3500+row,width,height);
                const auto pixels=colorGradeRead(renderer,device,context,output,scale);
                if(scale==1)baseline[row]=pixels;
                unsigned mismatches=0,maximum=0,firstSwatch=UINT_MAX,firstChannel=0;
                int firstActual=0,firstExpected=0;
                for(unsigned n=0;n<swatches.size();++n) {
                    const auto expected=scale==1?colorGradeOracle(cubePixels,swatches[n],flags):
                        std::array<uint8_t,3>{baseline[row][(size_t(3)*width+n*4+1)*4],
                                              baseline[row][(size_t(3)*width+n*4+1)*4+1],
                                              baseline[row][(size_t(3)*width+n*4+1)*4+2]};
                    for(unsigned y=2*scale;y<6*scale;++y)for(unsigned x=(n*4+1)*scale;x<(n*4+3)*scale;++x) {
                        const auto* p=pixels.data()+(size_t(y)*width*scale+x)*4;
                        require(p[3]==255,"Color-grade chain altered opaque scene alpha");
                        for(unsigned c=0;c<3;++c) {
                            const unsigned delta=unsigned(std::abs(int(p[c])-int(expected[c])));
                            maximum=(std::max)(maximum,delta);++comparisons;
                            if(delta>2) {
                                ++mismatches;
                                if(firstSwatch==UINT_MAX){firstSwatch=n;firstChannel=c;firstActual=p[c];firstExpected=expected[c];}
                            }
                        }
                    }
                }
                const auto* corner=pixels.data()+(size_t(3*scale)*width*scale+scale)*4;
                if(kind<5 && (corner[0]>2 || corner[1]>2 || corner[2]>2))passed=false;
                std::printf("ColorGradeChain: case=%s format=%u exponent=%d Final5=%u scale=%u black=%u,%u,%u maxDelta=%u mismatches=%u firstSwatch=%u channel=%u actual=%d expected=%d oracle=%s\n",
                    names[kind],lutFormat,lutExponent,flags,scale,corner[0],corner[1],corner[2],maximum,mismatches,firstSwatch,firstChannel,
                    firstActual,firstExpected,scale==1?"trilinear":"scale1");
                passed=(mismatches==0)&&passed;
            }
        }
    }
    context->ClearState();
    require(passed,"Resolved color-grade chain differs from logical LUT interpolation or scale1 output");
    std::printf("ColorGradeChain passed: format=%u exponent=%d, all CCFuser variants and retail0-2-4 chain; resolved append address modes; Final5 flags4/14; %u comparisons at scales1/2/3.\n",
                lutFormat,lutExponent,comparisons);
}

// Observe the constants actually bound for each draw. Alternating eligible
// and fallback resources catches stale masks in the renderer's upload cache;
// ClearState plus invalidateBindings catches stale context binding state.
static void colorGradeLookupState(ID3D11Device* device,ID3D11DeviceContext* context,
                                  const char* name,unsigned scale,unsigned expectedMask) {
    ComPtr<ID3D11Buffer> source,staging;
    context->PSGetConstantBuffers(3,1,source.GetAddressOf());
    require(bool(source),"Color-lookup constants were not bound at pixel b3");
    D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
    require(desc.ByteWidth==16,"Color-lookup constant-buffer size differs from its HLSL contract");
    desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateBuffer(&desc,nullptr,&staging),"Color-lookup constants staging");
    context->CopyResource(staging.Get(),source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Color-lookup constants readback");
    struct State {float inverseScale;uint32_t mask,padding[2];};
    static_assert(sizeof(State)==16);State state{};
    std::memcpy(&state,mapped.pData,sizeof(state));context->Unmap(staging.Get(),0);
    std::printf("ColorGradeState: case=%s scale=%u inverseScale=%g mask=%u expectedMask=%u padding=%u,%u\n",
                name,scale,state.inverseScale,state.mask,expectedMask,state.padding[0],state.padding[1]);
    require(state.mask==expectedMask && state.inverseScale==1.0f/float(scale) &&
            state.padding[0]==0 && state.padding[1]==0,
            "Color-lookup bound constants differ from the selected resource/sampler contract");
}
static void colorGradeEligibilityContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    const auto nonlinear=colorGradeCube(true);
    auto black=std::make_shared<ColorImage>();black->width=black->height=1;black->pixels={0,0,0,255};
    const auto cubeGeometry=colorGradeQuad(324,18,1280,720);
    const auto screenGeometry=colorGradeQuad(8,8,8,8);
    unsigned cases=0;
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        auto copy=colorGradeDraw(cubeGeometry,3901,1280,720);
        copy.fragmentName="XREngine_CCFuser";copy.fragmentFlags=0;
        copy.fragmentConstants[0]={1,1,1,1};copy.textures[1]=nonlinear;
        copy.samplers[1]=colorGradeSampler(true,0);
        WorldClear clear;clear.targets=copy.targets;clear.viewport=copy.viewport;clear.flags=1;
        renderer.clear(clear);require(renderer.draw(copy),"Eligibility source LUT copy rejected");
        const auto lookup=colorGradeResolve(renderer,copy,3902,324,18);
        auto final=colorGradeDraw(screenGeometry,3903,8,8);
        final.fragmentName="XREngine_Final5";final.fragmentFlags=4;
        final.fragmentConstants[0]={1,1,1,1};final.textures[0]=black;
        final.textureObjects[2]=lookup;final.samplers[0]=colorGradeSampler(false,2);
        final.samplers[2]=colorGradeSampler(true,2);
        const unsigned enabled=scale>1?4:0;
        auto run=[&](const char* name,const WorldDraw& draw,unsigned mask) {
            require(renderer.draw(draw),"Eligibility fixture draw rejected");
            colorGradeLookupState(device,context,name,scale,mask);++cases;
        };
        auto toggle=[&](const char* name,const WorldDraw& draw) {
            run(name,draw,0);run("eligible-after-fallback",final,enabled);
        };
        run("eligible-resolved",final,enabled);
        auto alternate=final;alternate.textures[2]=nonlinear;
        run("selected-resolved-with-CPU-owner",alternate,enabled);
        alternate=final;alternate.textureObjects[2]={};alternate.textures[2]=nonlinear;
        toggle("native-CPU-LUT",alternate);
        alternate=final;alternate.samplers[2].minLinear=false;
        toggle("point-minification",alternate);
        alternate=final;alternate.samplers[2].magLinear=false;
        toggle("point-magnification",alternate);
        alternate=final;alternate.samplers[2].valid=false;
        toggle("unknown-sampler",alternate);
        alternate=final;alternate.samplers[2].anisotropy=2;
        toggle("anisotropic-sampler",alternate);
        alternate=final;alternate.samplers[2].bias=.25f;
        toggle("biased-sampler",alternate);
        alternate=final;alternate.samplers[2].minLevel=alternate.samplers[2].maxLevel=1;
        toggle("nonbase-minimum-LOD",alternate);
        alternate=final;alternate.samplers[2].baseOnly=false;alternate.samplers[2].maxLevel=1;
        toggle("nonbase-maximum-LOD",alternate);
        alternate=final;alternate.textureObjects[2].mipLevels=2;
        toggle("declared-mip-chain",alternate);
        alternate=final;alternate.textureObjects[2].firstMip=1;
        toggle("nonbase-first-mip",alternate);
        alternate=final;alternate.fragmentFlags=0;
        toggle("Final5-without-rgbmap",alternate);
        alternate=final;alternate.samplers[2].mipLinear=false;
        run("point-mip-filter-at-base-LOD",alternate,enabled);
        alternate=final;alternate.samplers[2].baseOnly=false;
        run("linear-sampler-with-zero-LOD-range",alternate,enabled);
        alternate=final;alternate.samplers[2].bias=-0.0f;
        run("negative-zero-LOD-bias",alternate,enabled);
        // Descriptor mismatch remains an invalid binding; the renderer must
        // reject it instead of adjusting the selected resource to fit the LUT.
        alternate=final;alternate.textureObjects[2].width=323;
        require(!renderer.draw(alternate),"Mismatched LUT dimensions were accepted");++cases;
        alternate=final;alternate.textureObjects[2].faces=6;
        require(!renderer.draw(alternate),"Cubemap descriptor was accepted by a 2D LUT consumer");++cases;
        run("eligible-after-invalid-descriptor",final,enabled);
        context->ClearState();renderer.invalidateBindings();
        run("eligible-after-ClearState",final,enabled);
        alternate=copy;alternate.fragmentFlags=2;
        alternate.textures[0]=black;alternate.textureObjects[1]=lookup;alternate.textures[1].reset();
        alternate.samplers[0]=colorGradeSampler(false,2);alternate.samplers[1]=colorGradeSampler(true,2);
        run("CCFuser-resolved-append",alternate,scale>1?2:0);
        alternate.fragmentFlags=0;
        run("CCFuser-copy-is-spatial",alternate,0);
        run("Final5-after-CCFuser",final,enabled);
    }
    context->ClearState();
    std::printf("ColorGradeEligibility passed: %u selected-resource, sampler, consumer and context-transition cases at scales1/2/3.\n",cases);
}
