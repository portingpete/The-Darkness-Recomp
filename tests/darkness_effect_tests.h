// Original 822478C0 chooses TexEnvMode02 for two contiguous texture IDs.
// after-02 event3 expands UV1's lower-right atlas region over the whole output,
// while UV0 spans its entire texture. Both stages in the .fp use c0, not c1.
static void darknessFixedCompositeContract(WorldRendererD3D11& renderer) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=28;
    geometry->formats[0]=3;geometry->formats[1]=geometry->formats[2]=2;
    geometry->vertices.resize(4*28);geometry->indices={0,1,2,0,2,3};
    const float positions[4][3]{{-1,-1,.5f},{-1,1,.5f},{1,1,.5f},{1,-1,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    // Captured minima: half an original 1720x720 texel beyond the atlas midpoint.
    const float atlasMin[2]{std::bit_cast<float>(0x3f00130du),std::bit_cast<float>(0x3f002d83u)};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*28;
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned c=0;c<2;++c) {
            put(p+12+c*4,std::bit_cast<uint32_t>(uvs[v][c]));
            put(p+20+c*4,std::bit_cast<uint32_t>(atlasMin[c]+(1-atlasMin[c])*uvs[v][c]));
        }
    }
    using Rgba=std::array<uint8_t,4>;
    const Rgba sceneColors[4]{{208,96,144,255},{48,224,160,160},{112,64,240,80},{240,176,32,0}};
    const Rgba atlasColors[4]{{64,192,224,64},{224,112,80,255},{160,240,48,0},{96,48,192,192}};
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=2;
    for(const auto& color:sceneColors)scene->pixels.insert(scene->pixels.end(),color.begin(),color.end());
    auto atlas=std::make_shared<ColorImage>();atlas->width=atlas->height=4;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        const Rgba color=x>=2 && y>=2?atlasColors[(y-2)*2+x-2]:Rgba{7,251,13,17};
        atlas->pixels.insert(atlas->pixels.end(),color.begin(),color.end());
    }
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode02";
    draw.textureMask=worldFragmentTextureMask(draw.fragmentName,0);
    require(draw.textureMask==3,"Fixed composite omitted an original texture binding");
    draw.options.modes.fill(4);draw.options.modes[0]=draw.options.modes[1]=0;
    draw.options.coordinates[1]=1;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;
    draw.textures[0]=scene;draw.textures[1]=atlas;
    for(unsigned slot=0;slot<2;++slot) {
        draw.samplers[slot].valid=true;draw.samplers[slot].address.fill(2);
    }
    // Event3 writes RGBA, with alpha ALWAYS and framebuffer blending disabled.
    put(draw.attributes.data()+92,0x01100210);draw.attributes[97]=8;
    draw.attributes[144]=2;draw.attributes[145]=1;
    struct Case {EngineVector c0,color;};
    const Case cases[]{
        {{0,0,0,0},{1,1,1,1}}, // Exact captured modulation, white vertex constant.
        {{0,0,0,0},{.75f,.5f,.875f,.625f}},
        {{1,1,1,1},{.75f,.5f,.875f,.625f}},
        {{.25f,.5f,.75f,.375f},{.75f,.5f,.875f,.625f}}
    };
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    unsigned checks=0;
    for(auto extent:{std::array<uint32_t,2>{64,64},{128,48},{256,72}}) {
        draw.viewport={0,0,extent[0],extent[1]};draw.targets={2301+extent[0],0,0,0,0};
        WorldClear clear;clear.viewport=draw.viewport;clear.targets=draw.targets;clear.flags=1;
        clear.color={.125f,.25f,.375f,.875f};
        for(unsigned test=0;test<std::size(cases);++test) {
            draw.constants.vectors[10]=cases[test].color;draw.fragmentConstants[0]=cases[test].c0;
            // Deliberately disagree: the original second lrp still reads c0.
            for(unsigned c=0;c<4;++c)draw.fragmentConstants[1][c]=1-cases[test].c0[c];
            renderer.clear(clear);
            require(renderer.draw(draw),"Original two-stage fixed composite rejected");
            const auto pixels=renderer.readSurface(draw.targets[0],false);
            require(pixels.size()==size_t(extent[0])*extent[1]*8,"Fixed composite output extent changed");
            // Check every pixel, including all four corners: a quarter-screen
            // quad, shared UVs, reversed V or reading outside the atlas must fail.
            for(unsigned y=0;y<extent[1];++y)for(unsigned x=0;x<extent[0];++x) {
                const double u=(x+.5)/extent[0],v=(y+.5)/extent[1];
                const unsigned sceneIndex=(v>=.5?2:0)+(u>=.5?1:0);
                const unsigned ax=unsigned((atlasMin[0]+(1-atlasMin[0])*u)*4);
                const unsigned ay=unsigned((atlasMin[1]+(1-atlasMin[1])*v)*4);
                require(ax>=2 && ax<4 && ay>=2 && ay<4,"Fixed composite oracle left the atlas region");
                const Rgba samples[]{sceneColors[sceneIndex],atlasColors[(ay-2)*2+ax-2]};
                std::array<double,4> expected{};
                for(unsigned c=0;c<4;++c)expected[c]=cases[test].color[c];
                // .fp lines22..32: lrp(D.a,R,D) in RGB, preserve R.a;
                // then lrp(c0,that,R*D), independently for both samples.
                for(const auto& sample:samples)for(unsigned c=0;c<4;++c) {
                    const double texel=sample[c]/255.0,alpha=sample[3]/255.0;
                    const double mixed=c==3?expected[c]:alpha*expected[c]+(1-alpha)*texel;
                    const double weight=cases[test].c0[c];
                    expected[c]=weight*mixed+(1-weight)*expected[c]*texel;
                }
                uint16_t pixel[4];std::memcpy(pixel,pixels.data()+(size_t(y)*extent[0]+x)*8,8);
                for(unsigned c=0;c<4;++c) {
                    const double actual=half(pixel[c]);
                    if(!std::isfinite(actual) || std::abs(actual-expected[c])>=.002) {
                        std::fprintf(stderr,"DarknessFixedComposite extent=%ux%u case=%u pixel=%u,%u lane=%u actual=%g expected=%g\n",
                            extent[0],extent[1],test,x,y,c,actual,expected[c]);
                        throw std::runtime_error("Two-stage atlas composite differs from original TexEnvMode02");
                    }
                    ++checks;
                }
            }
        }
    }
    for(unsigned slot=0;slot<2;++slot) {
        auto missing=draw;missing.textures[slot].reset();
        require(!renderer.draw(missing),"Fixed composite accepted a missing required texture");
    }
    std::printf("DarknessFixedComposite: %u full-output RGBA checks, distinct atlas UVs, modulation and mixed c0 passed.\n",checks);
}

// The voice effect composites eight scene samples and three independent masks.
// Check actual GPU pixels across the whole output, including wide viewports.
static void darknessEffectContract(WorldRendererD3D11& renderer) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=28;
    geometry->formats[0]=3;geometry->formats[1]=geometry->formats[2]=2;
    geometry->vertices.resize(4*28);geometry->indices={0,1,2,0,2,3};
    const float positions[4][3]{{-1,-1,.5f},{-1,1,.5f},{1,1,.5f},{1,-1,.5f}};
    const float uvs[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*28;
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(positions[v][c]));
        for(unsigned stream=0;stream<2;++stream)for(unsigned c=0;c<2;++c)
            put(p+12+stream*8+c*4,std::bit_cast<uint32_t>(uvs[v][c]));
    }
    auto solid=[](uint8_t value) {
        auto image=std::make_shared<ColorImage>();image->width=image->height=1;
        image->pixels={value,value,value,255};return image;
    };
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=2;
    scene->pixels={32,64,96,255, 128,160,192,255, 64,128,224,255, 224,96,32,255};
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.material=WorldMaterial::post;draw.fragmentName="XREngine_RadialBlurInvert";
    draw.textureMask=worldFragmentTextureMask(draw.fragmentName,0);
    require(draw.textureMask==15,"Darkness effect capture omitted a scene/mask binding");
    draw.options.modes.fill(4);draw.options.modes[0]=draw.options.modes[1]=0;
    draw.options.coordinates[1]=1;
    for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    draw.attributes[97]=8;put(draw.attributes.data()+92,0x01100000);
    draw.fragmentConstants[0]={.5f,.5f,0,0};
    draw.fragmentConstants[1]={0,0,0,1}; // zero streak, full strength
    draw.fragmentConstants[4]={1,0,0,0}; // preserves each squared RGB lane
    draw.fragmentConstants[5]={1,1,1,1};
    draw.textures[0]=scene;draw.textures[1]=solid(255);draw.textures[2]=solid(0);
    // Point sampling makes each quadrant's analytic source color independent
    // of pixel-center offsets and the linear filter's cross-quadrant blend.
    for(unsigned slot=0;slot<4;++slot) {
        draw.samplers[slot].valid=true;draw.samplers[slot].address.fill(2);
    }
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    unsigned checks=0;
    for(auto extent:{std::array<uint32_t,2>{64,64},{128,48},{256,72}}) {
        draw.viewport={0,0,extent[0],extent[1]};draw.targets={1901+extent[0],0,0,0,0};
        WorldClear clear;clear.viewport=draw.viewport;clear.targets=draw.targets;clear.flags=1;
        for(uint8_t mask:{uint8_t(0),uint8_t(128),uint8_t(255)}) {
            draw.textures[3]=solid(mask);renderer.clear(clear);
            require(renderer.draw(draw),"Original Darkness voice fragment rejected");
            const auto pixels=renderer.readSurface(draw.targets[0],false);
            require(pixels.size()==size_t(extent[0])*extent[1]*8,"Darkness output extent changed");
            for(unsigned quadrant=0;quadrant<4;++quadrant) {
                const unsigned x=extent[0]*(quadrant%2?3:1)/4,y=extent[1]*(quadrant/2?3:1)/4;
                uint16_t pixel[4];std::memcpy(pixel,pixels.data()+(size_t(y)*extent[0]+x)*8,8);
                const double weight=mask/255.0;
                for(unsigned c=0;c<4;++c) {
                    const double color=scene->pixels[quadrant*4+c]/255.0;
                    const double expected=weight*(c==3?0:color*color)+(1-weight);
                    if(std::abs(half(pixel[c])-expected)>=.002) {
                        std::fprintf(stderr,"DarknessEffect extent=%ux%u mask=%u quadrant=%u lane=%u actual=%g expected=%g\n",
                            extent[0],extent[1],unsigned(mask),quadrant,c,half(pixel[c]),expected);
                        throw std::runtime_error("Darkness scene/mask pixel differs from source formula");
                    }
                    ++checks;
                }
            }
        }
    }
    for(unsigned slot=0;slot<4;++slot) {
        auto missing=draw;missing.textures[slot].reset();
        require(!renderer.draw(missing),"Darkness effect accepted a missing required mask");
    }
    std::printf("DarknessEffect: %u quadrant scene/mask RGBA checks at square, ultrawide and 32:9 extents passed.\n",checks);
    darknessFixedCompositeContract(renderer);
}
