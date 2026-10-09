// The shipped Xenon shadow projector reads both the scene depth and a resolved
// depth atlas. Its comparison box spans three logical texels at every scale;
// resolved atlases contribute every covered native texel to that box.
static void shadowProjectionPass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    WorldClear shadow;shadow.targets[4]=3101;shadow.viewport={0,0,64,64};shadow.flags=48;
    WorldResolve shadowCopy;shadowCopy.targets=shadow.targets;shadowCopy.flags=4;
    shadowCopy.rectangle={0,0,64,64};shadowCopy.destination={3104,0x3104000,64,64,23,0,1};
    WorldClear scene=shadow;scene.targets[4]=3102;scene.depth=.5f;renderer.clear(scene);
    WorldResolve sceneCopy=shadowCopy;sceneCopy.targets=scene.targets;
    sceneCopy.destination={3105,0x3105000,64,64,23,0,1};
    require(renderer.resolve(sceneCopy),"Scene depth resolve for shadow projector rejected");

    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;
    // Saved gameplay draws use stage-0 mode 1 for this projector.
    binding.descriptor.modes.fill(4);binding.descriptor.modes[0]=1;
    binding.descriptor.parameters[0][0]=20;
    const auto bytes=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|bytes[i*4+n];
    const std::array<EngineVector,4> projection{{{0,0,0,.5f},{0,0,0,.5f},{0,0,0,0},{0,0,0,1}}};
    for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)
        put(binding.constantBytes.data()+(20+row)*16+lane*4,std::bit_cast<uint32_t>(projection[row][lane]));
    auto draw=seed;require(prepareWorldVertexProgram(binding,draw.options,draw.constants),"Shadow projector vertex binding failed");
    draw.targets={3103,0,0,0,0};draw.material=WorldMaterial::post;
    draw.fragmentName="XREngine_ShadowProj";draw.fragmentFlags=8;draw.fragmentConstants={};
    draw.fragmentConstants[1]={0,0,1,-1}; // Finite scene-depth reconstruction.
    draw.fragmentConstants[5]={0,0,0,.5f}; // Projected depth becomes 1 - .5.
    draw.fragmentConstants[6]={0,0,0,1};
    draw.fragmentConstants[9]={1.0f/64,1.0f/64,0,0};
    draw.textureObjects[0]=shadowCopy.destination;draw.textureObjects[1]=sceneCopy.destination;
    draw.textures[0].reset();draw.textures[1].reset();
    for(unsigned slot=0;slot<2;++slot) {
        auto& sampler=draw.samplers[slot];sampler.valid=sampler.lodValid=true;
        sampler.minLevel=sampler.maxLevel=0;sampler.address.fill(2);
    }
    draw.attributes.fill(0);put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    WorldClear target;target.targets=draw.targets;target.viewport=draw.viewport;target.flags=1;
    auto halfFloat=[](uint16_t h) {
        const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);
    };
    auto checkProjection=[&](double expected,const char* message) {
        renderer.clear(target);require(renderer.draw(draw),"Original shadow projector draw rejected");
        const auto pixels=renderer.readSurface(3103,false);
        const unsigned side=64*scale;require(pixels.size()==size_t(side)*side*8,"Shadow output scale differs");
        uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+(size_t(side/2)*side+side/2)*8,8);
        require(halfFloat(rgba[0])>=0 && halfFloat(rgba[0])<=1 &&
                halfFloat(rgba[3])>=0 && halfFloat(rgba[3])<=1,
                "Shadow coverage or remaining light exceeds zero to one");
        require(std::abs(halfFloat(rgba[0])-expected)<.01 &&
                std::abs(halfFloat(rgba[3])-(1-expected))<.01,message);
    };
    // ShadowMapStep in FPInclude_Xenon returns step(receiver, sampledDepth).
    // With reversed depth, zero is empty/far and a larger depth is an occluder.
    // Red is shadow coverage; alpha is the remaining light, not the reverse.
    const float depths[]{.25f,.75f,.25f,.25f,.25f,0.0f,1.0f};
    const double coverage[]{0.0,1.0,.5,1.0/6,5.0/6,0.0,1.0};
    for(unsigned pattern=0;pattern<std::size(depths);++pattern) {
        shadow.depth=depths[pattern];renderer.clear(shadow);
        if(pattern>=2 && pattern<=4) {
            auto right=shadow;right.rectangle=std::array<int32_t,4>{32,0,64,64};right.depth=.75f;
            renderer.clear(right);
        }
        require(renderer.resolve(shadowCopy),"Shadow atlas depth resolve rejected");
        // One logical pixel off the seam must still see the opposite side at
        // 2x/3x. A filter accidentally measured in physical pixels will not.
        draw.fragmentConstants[3][3]=pattern==3?-1.0f/64:pattern==4?1.0f/64:0;
        checkProjection(coverage[pattern],"Shadow comparison or logical PCF coverage differs from Xenon");
    }
    // Equality must retain step()'s inclusive comparison. Use exact endpoint
    // depths so D24 quantization cannot turn equality into an ordered case.
    for(float depth:{0.0f,1.0f}) {
        shadow.depth=depth;renderer.clear(shadow);
        require(renderer.resolve(shadowCopy),"Equal-depth shadow resolve rejected");
        draw.fragmentConstants[5][3]=1-depth;
        checkProjection(1,"Shadow comparison lost inclusive equality");
    }
    draw.fragmentConstants[5][3]=.5f;
    // An isolated caster must not darken the cleared rectangle around it.
    shadow.depth=0;renderer.clear(shadow);
    auto caster=shadow;caster.rectangle=std::array<int32_t,4>{24,24,40,40};caster.depth=.75f;
    renderer.clear(caster);require(renderer.resolve(shadowCopy),"Isolated caster resolve rejected");
    for(const auto& offset:std::array<std::array<float,2>,5>{{{0,0},{-.25f,0},{.25f,0},{0,-.25f},{0,.25f}}}) {
        draw.fragmentConstants[3][3]=offset[0];draw.fragmentConstants[4][3]=offset[1];
        checkProjection(offset[0]==0 && offset[1]==0?1:0,"Cleared shadow atlas casts a rectangular shadow");
    }

    // Constant scene depth and projection let us move the lookup independently
    // of raster position. A binary atlas exposes comparison filtering directly:
    // averaging depths before comparison would erase these fractional shadows.
    // The original separable kernel has weights [1-f,1,1,f] in each direction,
    // normalized by nine. Its outer weights must vary with subtexel position;
    // sixteen uniformly weighted comparisons give visible stair steps instead.
    std::array<uint8_t,64*64> mask{};
    auto uploadMask=[&](unsigned pattern) {
        shadow.depth=.25f;renderer.clear(shadow);
        for(unsigned y=0;y<64;++y) {
            const unsigned first=pattern==0?32:pattern==1?(y>=32?0:64):
                                 pattern==2?y:(y>=32?32:64);
            for(unsigned x=0;x<64;++x)mask[y*64+x]=uint8_t(x>=first);
            if(first==64)continue;
            auto row=shadow;row.rectangle=std::array<int32_t,4>{int32_t(first),int32_t(y),64,int32_t(y+1)};
            row.depth=.75f;renderer.clear(row);
        }
        require(renderer.resolve(shadowCopy),"Subtexel shadow atlas resolve rejected");
    };
    // Integrate the area of native unit texel squares intersecting the original
    // box. This oracle does not reproduce the shader's endpoint-weight loop.
    auto boxOracle=[&](double px,double py,uint8_t address,bool border,const auto& texel) {
        const double left=(px-1)*scale,top=(py-1)*scale;
        const double right=left+3*scale,bottom=top+3*scale;
        const int side=int(64*scale);
        double coverage=0;
        for(int y=int(std::floor(top));y<int(std::ceil(bottom));++y)
            for(int x=int(std::floor(left));x<int(std::ceil(right));++x) {
                const double area=((std::min)(right,double(x+1))-(std::max)(left,double(x)))*
                                  ((std::min)(bottom,double(y+1))-(std::max)(top,double(y)));
                int column=x,row=y;
                const bool outside=column<0 || column>=side || row<0 || row>=side;
                double sample=0;
                if(address==6 && outside)sample=border?1:0;
                else {
                    if(address==0) {column=(column%side+side)%side;row=(row%side+side)%side;}
                    else {column=(std::clamp)(column,0,side-1);row=(std::clamp)(row,0,side-1);}
                    sample=texel(column,row);
                }
                coverage+=area*sample;
            }
        return coverage/(9*scale*scale);
    };
    auto oracle=[&](double px,double py,uint8_t address,bool border) {
        return boxOracle(px,py,address,border,[&](int x,int y) {
            return mask[size_t(y/scale)*64+unsigned(x)/scale];
        });
    };
    unsigned phaseChecks=0,addressChecks=0;
    auto checkLookup=[&](double px,double py,uint8_t address,bool border,bool linear,unsigned pattern) {
        // p = uv / logicalPitch - .5. These binary fractions are exact in the
        // shader's float constants, including phases around cell boundaries.
        draw.fragmentConstants[3][3]=float((px+.5)/64-.5);
        draw.fragmentConstants[4][3]=float(.5-(py+.5)/64);
        auto& sampler=draw.samplers[0];sampler.address.fill(address);sampler.border=uint8_t(border);
        sampler.minLinear=sampler.magLinear=sampler.mipLinear=linear;
        const auto expected=oracle(px,py,address,border);
        try {
            checkProjection(expected,"Subtexel shadow comparison weights or sampler addressing differ from Xenon");
        } catch(...) {
            std::fprintf(stderr,"ShadowProjection%u pattern=%u logical=%g,%g address=%u border=%u linear=%u expected=%g\n",
                         scale,pattern,px,py,unsigned(address),unsigned(border),unsigned(linear),expected);
            throw;
        }
    };
    // Sweep both axes independently and cross p=32. A fixed UV fixture can
    // pass while edges still jump or lose their phase at native raster scales.
    constexpr double phases[]{0,.125,.5,.875,1};
    for(unsigned pattern=0;pattern<4;++pattern) {
        uploadMask(pattern);
        for(bool linear:{false,true})for(double fy:phases)for(double fx:phases) {
            checkLookup(31+fx,31+fy,2,false,linear,pattern);++phaseChecks;
        }
    }
    // Keep the corner mask, whose opposite atlas edges differ in both axes.
    // Probe every edge and two corners while the footprint straddles the atlas,
    // including coordinates beyond one. Point overrides must retain addressing
    // and the border depth rather than silently impose clamp or a black border.
    constexpr std::array<std::array<double,2>,8> edgePositions{{
        {-.125,31.375},{63.125,31.375},{31.375,-.125},{31.375,63.125},
        {-.125,-.375},{63.125,63.375},{64.125,64.375},{-.125,63.375}}};
    struct Address {uint8_t mode;bool border;};
    constexpr Address addresses[]{{2,false},{0,false},{6,false},{6,true}};
    for(bool linear:{false,true})for(const auto& address:addresses)for(const auto& position:edgePositions) {
        checkLookup(position[0],position[1],address.mode,address.border,linear,3);++addressChecks;
    }

    // Render a diagonal caster instead of clearing logical rectangles. At 2x/3x
    // its edge has distinct depths inside logical cells; one logical-center
    // lookup discards that coverage and jumps as the projected phase moves.
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=3;geometry->stride=16;geometry->formats[0]=4;
    geometry->vertices.resize(48);geometry->indices={0,1,2};
    constexpr float corners[3][2]{{25.1875f,26.0625f},{39.5625f,36.4375f},{36.125f,40.0625f}};
    for(unsigned i=0;i<3;++i) {
        const EngineVector position{corners[i][0]/32-1,1-corners[i][1]/32,.75f,1};
        for(unsigned lane=0;lane<4;++lane)
            put(geometry->vertices.data()+i*16+lane*4,std::bit_cast<uint32_t>(position[lane]));
    }
    auto casterDraw=draw;casterDraw.material=WorldMaterial::depth;casterDraw.fragmentName.clear();
    casterDraw.fragmentFlags=0;casterDraw.targets={0,0,0,0,3101};casterDraw.depthRange={0,1,0,0};
    casterDraw.geometry.vertices=casterDraw.geometry.indices=geometry;
    casterDraw.geometry.firstIndex=0;casterDraw.geometry.indexCount=3;
    casterDraw.constants.vectors[7]={};
    for(unsigned row=0;row<4;++row) {
        casterDraw.constants.vectors[row]={};casterDraw.constants.vectors[row][row]=1;
    }
    casterDraw.attributes.fill(0);put(casterDraw.attributes.data()+92,6);casterDraw.attributes[96]=8;
    shadow.depth=.25f;renderer.clear(shadow);
    require(renderer.draw(casterDraw),"Native diagonal shadow caster rejected");
    const auto nativeDepth=renderer.readSurface(3101,true);
    const unsigned nativeSide=64*scale;
    require(nativeDepth.size()==size_t(nativeSide)*nativeSide*4,"Native diagonal shadow extent differs");
    auto nativeTexel=[&](int x,int y) {
        uint32_t packed=0;std::memcpy(&packed,nativeDepth.data()+(size_t(y)*nativeSide+unsigned(x))*4,4);
        return (packed&0xFFFFFF)>=0x800000?1.0:0.0;
    };
    unsigned partialCells=0;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x) {
        unsigned occupied=0;
        for(unsigned yy=0;yy<scale;++yy)for(unsigned xx=0;xx<scale;++xx)
            occupied+=nativeTexel(int(x*scale+xx),int(y*scale+yy))!=0;
        partialCells+=occupied>0 && occupied<scale*scale;
    }
    require(scale==1 || partialCells>0,"Diagonal caster failed to exercise coverage inside logical cells");
    require(renderer.resolve(shadowCopy),"Native diagonal shadow resolve rejected");
    draw.fragmentConstants[5][3]=.5f;
    auto& nativeSampler=draw.samplers[0];nativeSampler.address.fill(2);nativeSampler.border=0;
    unsigned nativeChecks=0,sparseDifferences=0;
    auto checkNative=[&](double px,double py) {
        draw.fragmentConstants[3][3]=float((px+.5)/64-.5);
        draw.fragmentConstants[4][3]=float(.5-(py+.5)/64);
        const double expected=boxOracle(px,py,2,false,nativeTexel);
        // Independent legacy sparse result makes the fixture prove its value:
        // an implementation that merely retains logical anchors must fail it.
        const int ix=int(std::floor(px)),iy=int(std::floor(py));
        const double wx[]{1-(px-ix),1,1,px-ix},wy[]{1-(py-iy),1,1,py-iy};
        double sparse=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
            const int column=int((ix+x-1+.5)*scale),row=int((iy+y-1+.5)*scale);
            sparse+=nativeTexel(column,row)*wx[x]*wy[y]/9;
        }
        sparseDifferences+=std::abs(sparse-expected)>.01;
        try {
            checkProjection(expected,"Shadow projector skipped native diagonal caster coverage");
        } catch(...) {
            std::fprintf(stderr,"ShadowNative%u logical=%g,%g expected=%g legacySparse=%g partialCells=%u\n",
                         scale,px,py,expected,sparse,partialCells);
            throw;
        }
        ++nativeChecks;
    };
    for(bool linear:{false,true})for(double fy:phases)for(double fx:phases) {
        nativeSampler.minLinear=nativeSampler.magLinear=nativeSampler.mipLinear=linear;
        checkNative(31+fx,31+fy);
    }
    require(scale==1 || sparseDifferences>0,"Native shadow regression cannot distinguish sparse logical filtering");

    // Repeated logical depth blocks cannot distinguish point from linear reads
    // at the anchored centers. This atlas instead has two physical texels per
    // logical texel: every tap is between four checker cells. Linear depth reads
    // return .5 and give the opposite comparison for each receiver below, while
    // point reads select matching-parity cells at every tap and remain binary.
    draw.textureObjects[0]={};draw.fragmentConstants[3][3]=draw.fragmentConstants[4][3]=0;
    auto& sampler=draw.samplers[0];sampler.address.fill(2);sampler.border=0;
    for(bool inverse:{false,true}) {
        auto checker=std::make_shared<ColorImage>();checker->width=checker->height=128;
        checker->authoredMips=true;checker->pixels.resize(128*128*4);
        for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x) {
            const uint8_t depth=(((x+y)&1)!=unsigned(inverse))?64:191;
            const size_t index=(size_t(y)*128+x)*4;
            checker->pixels[index]=checker->pixels[index+1]=checker->pixels[index+2]=depth;
            checker->pixels[index+3]=255;
        }
        draw.textures[0]=checker;
        const float receiver=inverse?.4f:.6f;draw.fragmentConstants[5][3]=1-receiver;
        // CPU point comparison supplies the result independently of the PCF
        // accumulation. All sixteen anchors select this same checker parity.
        const double expected=double(checker->pixels[(61*128+61)*4])/255>=receiver?1:0;
        for(bool linear:{false,true}) {
            sampler.minLinear=sampler.magLinear=sampler.mipLinear=linear;
            checkProjection(expected,"Shadow projector filtered raw depths before comparison");
        }
    }
    // Rebind the resolved atlas after CPU fallback. Cached constants must return
    // to native coverage; the CPU checker above also catches stale native scale.
    draw.textures[0].reset();draw.textureObjects[0]=shadowCopy.destination;
    draw.fragmentConstants[5][3]=.5f;
    nativeSampler.minLinear=nativeSampler.magLinear=nativeSampler.mipLinear=false;
    checkNative(31.125,31.875);
    // Invalid logical pitches must reject before touching the rendered target,
    // including when a valid projector and its bindings are already cached.
    const auto unchanged=renderer.readSurface(3103,false);
    const float invalidPitches[]{0,-1,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()};
    for(unsigned axis=0;axis<2;++axis)for(float pitch:invalidPitches) {
        auto invalid=draw;invalid.fragmentConstants[9][axis]=pitch;
        require(!renderer.draw(invalid),"Invalid logical shadow pitch was accepted");
        require(renderer.readSurface(3103,false)==unchanged,"Rejected shadow pitch changed the rendered output");
        require(renderer.draw(draw),"Valid shadow projector did not recover after invalid pitch");
        require(renderer.readSurface(3103,false)==unchanged,"Invalid shadow pitch changed subsequent cached rendering");
    }
    std::printf("ShadowProjection%u: depth polarity, equality, isolated caster, %u subtexel edge/corner, %u address, %u native diagonal phases, raw-depth filtering, binding reset and invalid-pitch checks passed.\n",
                scale,phaseChecks,addressChecks,nativeChecks);
}
