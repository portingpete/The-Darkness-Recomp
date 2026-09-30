#pragma once

// Use the original Xenon depth reconstruction and eight perturbed depth taps
// from WClientMod_DV5_0.fp, with controlled noise that cannot cancel itself.
// The noise coordinates are held fixed independently of screen position; the
// depth discontinuity, pulse intensity and distance fades remain live inputs.
static void darknessVisionGlowContract(WorldRendererD3D11& renderer,
    const std::shared_ptr<StoredGeometry>& geometry,unsigned scale) {
    constexpr unsigned width=64,height=24;
    constexpr double foreground=32.0/255;
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=1;
    scene->pixels={32,32,32,255};
    auto pulse=std::make_shared<ColorImage>();pulse->width=pulse->height=1;
    pulse->pixels={255,255,255,255};
    auto perturbation=std::make_shared<ColorImage>();perturbation->width=perturbation->height=4;
    perturbation->pixels.resize(4*4*4);
    // TimeLevels=(.2,.6): the first noise lookup hits (0,1), its
    // subtracted lookup clamps to (0,0). Thus t1a=(64,0,128,0)/255.
    perturbation->pixels[(1*4)*4]=64;perturbation->pixels[(1*4)*4+2]=128;
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    auto saturate=[](double value){return (std::clamp)(value,0.0,1.0);};
    struct Scenario {const char* name;float left,right,intensity;double expectedEdgeRatio;};
    constexpr Scenario scenarios[]{
        {"gold-edge",100,200,1,1},{"intensity-zero",100,200,0,0},
        {"near-fade",60,120,1,.45},{"near-suppressed",20,40,1,0},
        {"far-fade",600,900,1,.6}
    };
    unsigned checks=0,glowing=0,discarded=0;
    double fullEdge=0;
    for(const auto& scenario:scenarios) {
        WorldClear depth;depth.targets[4]=3912;depth.viewport={0,0,width,height};depth.flags=16;
        // Physical perspective constants F=1, B=1000. Retain true R32 sampled
        // depth through a D24 resolve instead of quantizing it to an RGBA8 image.
        auto storedDepth=[](double distance){return float((1000.0/distance-1)/999.0);};
        depth.depth=storedDepth(scenario.left);renderer.clear(depth);
        auto right=depth;right.depth=storedDepth(scenario.right);
        right.rectangle=std::array<int32_t,4>{int32_t(width/2),0,int32_t(width),int32_t(height)};
        renderer.clear(right);
        const auto depthBytes=renderer.readSurface(3912,true);
        require(depthBytes.size()==size_t(width)*height*scale*scale*4,"DV5 stepped depth dimensions differ");
        uint32_t packedLeft=0,packedRight=0;
        std::memcpy(&packedLeft,depthBytes.data(),4);
        std::memcpy(&packedRight,depthBytes.data()+(width*scale-1)*4,4);
        const double sampledDepth[]{double(float(double(packedLeft&0xffffff)/0xffffff)),
                                    double(float(double(packedRight&0xffffff)/0xffffff))};
        // Equivalent geometric inverse of the original projection, independent
        // of the fragment translator's reciprocal and temporary-register order.
        const double distance[]{1000/(1+999*sampledDepth[0]),1000/(1+999*sampledDepth[1])};
        WorldResolve resolve;resolve.targets=depth.targets;resolve.viewport=depth.viewport;
        resolve.flags=4;resolve.rectangle={0,0,width,height};
        resolve.destination={3913,0x3913000,width,height,23,0,1};
        require(renderer.resolve(resolve),"DV5 stepped depth resolve rejected");
        WorldDraw draw;draw.geometry={geometry,geometry,0,6};draw.material=WorldMaterial::post;
        draw.fragmentName="WClientMod_DV5_0";draw.textureMask=0x17;
        draw.options.modes.fill(4);draw.options.modes[0]=draw.options.modes[2]=0;
        draw.options.coordinates={0,1,2,3,4,5,6,7};
        for(unsigned lane=0;lane<4;++lane)draw.constants.vectors[lane][lane]=1;
        draw.textures[0]=scene;draw.textures[2]=pulse;draw.textures[4]=perturbation;
        draw.textureObjects[1]=resolve.destination;
        for(auto& sampler:draw.samplers) {sampler.valid=true;sampler.address.fill(2);}
        draw.fragmentConstants[0]={1.f/width,1.f/height,0,0};
        draw.fragmentConstants[1]={.2f,.6f,0,scenario.intensity};
        draw.fragmentConstants[2]={1,1000,.001f,0};
        draw.fragmentConstants[3]={2000,1001,2000,1998};
        draw.fragmentConstants[4]={0,0,1,1};
        // Zero V2W rows make the authored perturbation samples deterministic;
        // the view-space distance still drives the pulse and all depth fades.
        draw.viewport=depth.viewport;draw.targets[0]=3911;draw.targets[4]=3914;
        draw.depthRange={1,0,0,0};
        // Captured DV5_0 writes a coverage depth mask without testing depth.
        // DV5_1 then restores only the pixels discarded by the first stage.
        put(draw.attributes.data()+92,0x01100214);draw.attributes[96]=4;draw.attributes[97]=8;
        WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
        clear.color={.0625f,.125f,.25f,.75f};renderer.clear(clear);
        auto maskClear=clear;maskClear.flags=16;
        // A failing stored comparison must not block writes when testing is off.
        maskClear.depth=scenario.intensity==0?1.f:0.f;renderer.clear(maskClear);
        require(renderer.draw(draw),"DV5 nonuniform edge-glow draw rejected");
        const auto pixels=renderer.readSurface(3911,false);
        require(pixels.size()==size_t(width)*height*scale*scale*8,"DV5 glow output dimensions differ");
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const double logicalX=(x+.5)/scale;
            const double center=distance[logicalX>=width/2];
            const bool killed=center>750;
            bool stable=true;double edgeSum=0,halfMinimum=center;
            // Source perturb1=(25,52,35,52), perturb2=(25,12,15,12).
            // Both use t1a; its zero Y/W components remove vertical offsets.
            constexpr double tapX[]{25*64.0/255,35*128.0/255,25*64.0/255,15*128.0/255};
            for(unsigned halfStep=0;halfStep<2;++halfStep)for(double tap:tapX) {
                const double sampleX=logicalX+tap*(200.0001/center)*(halfStep?.5:1);
                stable&=std::abs(sampleX-width/2)>.01;
                const double neighbor=distance[sampleX>=width/2];
                edgeSum+=(std::min)(25.5,std::abs(neighbor-center)*saturate(neighbor*.02-.75));
                // The source overwrites the full-step minimum with the
                // half-step minimum before its final far-distance fade.
                if(halfStep)halfMinimum=(std::min)(halfMinimum,neighbor);
            }
            // White pulse samples yield (sat(2-.75)+sat(4-3.5))*2.5=3.75.
            const double amount=(std::min)(.5,saturate(edgeSum*.001))*3.75*scenario.intensity*
                saturate(3-.004*halfMinimum)*saturate(.02*center-.75);
            const double glow[]{.35*amount,.35*std::pow(amount,1.5),.35*std::pow(amount,2.5)};
            uint16_t pixel[4]{};std::memcpy(pixel,pixels.data()+(size_t(y)*width*scale+x)*8,8);
            if(!stable && !killed)continue; // Avoid point-sample boundary sensitivity to reciprocal rounding.
            for(unsigned channel=0;channel<4;++channel) {
                const double expected=killed?clear.color[channel]:channel==3?1:
                    foreground+glow[channel]*(1-2*foreground);
                if(!std::isfinite(half(pixel[channel])) || std::abs(half(pixel[channel])-expected)>.0025) {
                    std::fprintf(stderr,"DarknessVisionGlow scale=%u case=%s pixel=%u,%u channel=%u actual=%.9g expected=%.9g center=%.9g edge=%.9g\n",
                        scale,scenario.name,x,y,channel,half(pixel[channel]),expected,center,edgeSum);
                    require(false,"DV5 gold edge glow differs from original depth-tap, pulse or fade math");
                }
                ++checks;
            }
            glowing+=!killed && amount>.01;discarded+=killed;
        }
        uint16_t edge[4]{};
        std::memcpy(edge,pixels.data()+((size_t(height*scale/2)*width*scale)+(width*scale/2-1))*8,8);
        const double redGlow=half(edge[0])-foreground;
        if(scenario.expectedEdgeRatio==1) {
            require(redGlow>.15 && half(edge[0])>half(edge[1]) && half(edge[1])>half(edge[2]),
                    "DV5 fixture failed to produce visible gold-colored edge glow");
            fullEdge=redGlow;
        } else require(std::abs(redGlow/fullEdge-scenario.expectedEdgeRatio)<.01,
                       "DV5 intensity or near/far fade did not control the visible edge");
        const auto mask=renderer.readSurface(3914,true);
        for(unsigned x=0;x<width*scale;++x) {
            uint32_t packed=0;std::memcpy(&packed,mask.data()+x*4,4);
            const bool killed=distance[x>=width*scale/2]>750;
            require(std::abs(double(packed&0xffffff)/0xffffff-(killed?0:.5))<1e-6,
                    "DV5 first stage lost its depth-write-only coverage mask");
        }
        auto fill=draw;fill.fragmentName="WClientMod_DV5_1";fill.textureMask=1;
        fill.constants.vectors[2][3]=.25f; // Further quad: reversed depth .25.
        put(fill.attributes.data()+92,0x01100212);
        require(renderer.draw(fill),"DV5 second-stage background fill rejected");
        const auto composite=renderer.readSurface(3911,false);
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const size_t offset=(size_t(y)*width*scale+x)*8;
            if(distance[x>=width*scale/2]<=750)
                require(std::memcmp(composite.data()+offset,pixels.data()+offset,8)==0,
                        "DV5 second stage overwrote the gold glow from the first stage");
            else {
                uint16_t restored[4]{};std::memcpy(restored,composite.data()+offset,8);
                for(unsigned channel=0;channel<4;++channel)
                    require(std::abs(half(restored[channel])-(channel==3?1:foreground))<.001,
                            "DV5 second stage did not restore a far-discarded scene pixel");
            }
        }
    }
    require(glowing>100*scale*scale && discarded==width*height*scale*scale/2,
            "DV5 glow fixture did not exercise both visible edges and the far discard");
    std::printf("DarknessVisionGlow%u: %u RGBA oracle checks, %u glowing pixels, %u far-discarded pixels; gold edge, intensity zero, near/far fades.\n",
                scale,checks,glowing,discarded);
}

// Exercise both original DV5 stages on a scene and a lower-right effect atlas.
// Uniform depth/noise cancels edge glow, giving independent analytic RGB
// results. A second depth selects the original Xenon far-plane discard.
static void darknessVisionContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=44;geometry->formats[0]=3;
    for(unsigned s=1;s<=4;++s)geometry->formats[s]=2;
    geometry->vertices.resize(4*44);geometry->indices={0,1,2,0,2,3};
    const float uv[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*44;
        const float position[]{uv[v][0]*2-1,1-uv[v][1]*2,.5f};
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(position[c]));
        for(unsigned s=0;s<4;++s)for(unsigned c=0;c<2;++c) {
            const float value=s==0?uv[v][c]:s==2?uv[v][c]*2-1:.5f+.5f*uv[v][c];
            put(p+12+s*8+c*4,std::bit_cast<uint32_t>(value));
        }
    }
    using Rgba=std::array<uint8_t,4>;
    const Rgba colors[]{{32,64,96,192},{96,128,160,192},{160,96,32,192},{128,64,32,192}};
    const Rgba effects[]{{16,32,48,64},{48,64,16,96},{64,16,32,128},{32,48,64,160}};
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=2;
    for(const auto& color:colors)scene->pixels.insert(scene->pixels.end(),color.begin(),color.end());
    auto atlas=std::make_shared<ColorImage>();atlas->width=atlas->height=4;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        const auto color=x>=2 && y>=2?effects[(y-2)*2+x-2]:Rgba{255,0,255,255};
        atlas->pixels.insert(atlas->pixels.end(),color.begin(),color.end());
    }
    auto zero=std::make_shared<ColorImage>();zero->width=zero->height=1;zero->pixels={0,0,0,255};
    auto one=std::make_shared<ColorImage>(*zero);one->pixels={255,255,255,255};
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    unsigned checks=0;
    for(unsigned scale:{1u,2u,3u}) {
        WorldRendererD3D11 renderer(device,context,scale);
        for(unsigned stage:{0u,1u})for(unsigned flags:{0u,1u,2u})for(bool beyondFarPlane:{false,true}) {
            WorldDraw draw;draw.geometry={geometry,geometry,0,6};draw.material=WorldMaterial::post;
            draw.fragmentName=stage==0?"WClientMod_DV5_0":"WClientMod_DV5_1";draw.fragmentFlags=flags;
            draw.textureMask=worldFragmentTextureMask(draw.fragmentName,flags);
            require(draw.textureMask==(stage==0?(flags?0x37:0x17):(flags?3:1)),"DV5 capture mask is incomplete");
            draw.options.modes.fill(4);draw.options.modes[0]=0;
            draw.options.coordinates={0,1,2,3,4,5,6,7};
            if(stage==0)draw.options.modes[2]=0;
            if(flags)draw.options.modes[stage==0?3:1]=0;
            for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
            draw.textures[0]=scene;
            if(stage==0) {draw.textures[1]=beyondFarPlane?one:zero;draw.textures[2]=draw.textures[4]=zero;}
            if(flags)draw.textures[stage==0?5:1]=atlas;
            for(auto& sampler:draw.samplers) {sampler.valid=true;sampler.address.fill(2);}
            draw.fragmentConstants[0]={1.f/64,1.f/24,0,0};
            draw.fragmentConstants[1]={.1f,.2f,.3f,1};
            draw.fragmentConstants[3]={2,0,2400,-2};
            draw.fragmentConstants[4]={0,0,1,1};
            for(unsigned c=0;c<3;++c)draw.fragmentConstants[5+c][c]=1;
            draw.viewport={0,0,64,24};draw.targets={3901,0,0,0,0};
            put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
            WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;clear.color={.25f,.125f,.5f,.75f};
            renderer.clear(clear);require(renderer.draw(draw),"Original Darkness Vision shader rejected");
            const auto pixels=renderer.readSurface(3901,false);
            require(pixels.size()==64*24*scale*scale*8,"DV5 output size changed");
            for(unsigned y=0;y<24*scale;++y)for(unsigned x=0;x<64*scale;++x) {
                const unsigned quadrant=(y>=12*scale?2:0)+(x>=32*scale?1:0);
                uint16_t pixel[4];std::memcpy(pixel,pixels.data()+(size_t(y)*64*scale+x)*8,8);
                for(unsigned c=0;c<4;++c) {
                    double expected=colors[quadrant][c]/255.0;
                    if(c==3 && stage==0)expected=1;
                    if(c<3 && flags==1)expected+=effects[quadrant][c]/255.0;
                    if(c<3 && flags==2)expected*=effects[quadrant][c]/255.0;
                    if(stage==0 && beyondFarPlane)expected=clear.color[c];
                    require(std::isfinite(half(pixel[c])) && std::abs(half(pixel[c])-expected)<.002,
                            "Darkness Vision differs from scene/atlas blend or Xenon depth discard");
                    ++checks;
                }
            }
        }
        darknessVisionGlowContract(renderer,geometry,scale);
        context->ClearState();
    }
    std::printf("DarknessVision: %u RGBA checks; both stages, all authored variants, atlas expansion, far discard, scale 1/2/3.\n",checks);
}
