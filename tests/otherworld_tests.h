#pragma once

static double otherworldHalf(uint16_t value) {
    const unsigned exponent=(value>>10)&31,mantissa=value&1023;
    return std::ldexp(double(exponent?1024+mantissa:mantissa),int(exponent?exponent:1)-25)*(value&0x8000?-1:1);
}
static std::shared_ptr<StoredGeometry> otherworldQuad(bool atlas) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=36;geometry->formats[0]=3;
    geometry->formats[1]=geometry->formats[2]=geometry->formats[3]=2;
    geometry->vertices.resize(4*36);geometry->indices={0,1,2,0,2,3};
    const float uv[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned vertex=0;vertex<4;++vertex) {
        auto* bytes=geometry->vertices.data()+vertex*36;
        const float position[]{uv[vertex][0]*2-1,1-uv[vertex][1]*2,.5f};
        for(unsigned lane=0;lane<3;++lane)put(bytes+lane*4,std::bit_cast<uint32_t>(position[lane]));
        for(unsigned stream=0;stream<3;++stream)for(unsigned lane=0;lane<2;++lane) {
            const float coordinate=stream==2?uv[vertex][lane]*2-1:
                stream==1 && atlas?.5f+.5f*uv[vertex][lane]:uv[vertex][lane];
            put(bytes+12+stream*8+lane*4,std::bit_cast<uint32_t>(coordinate));
        }
    }
    return geometry;
}
static WorldDraw otherworldDraw(const std::shared_ptr<StoredGeometry>& geometry,
    const char* program,uint32_t target) {
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};
    draw.targets={target,0,0,0,0};draw.viewport={0,0,64,24};
    draw.material=WorldMaterial::post;draw.fragmentName=program;
    draw.textureMask=worldFragmentTextureMask(program,0);
    draw.options.modes.fill(4);draw.options.modes[0]=0;
    draw.options.modes[std::strcmp(program,"WClientMod_OW1_1")==0?2:1]=0;
    for(unsigned stage=0;stage<8;++stage)draw.options.coordinates[stage]=uint8_t(stage);
    for(unsigned lane=0;lane<4;++lane)draw.constants.vectors[lane][lane]=1;
    for(auto& sampler:draw.samplers) {sampler.valid=true;sampler.address.fill(2);}
    put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    return draw;
}

// The real sewer failure leaves a smaller camera view in the upper-left of
// the scene. Restore a GPU-resolved, nonuniform scene through the original
// second OW1 stage, using independent UVs for a poisoned effect atlas.
static void otherworldCompositeContract(WorldRendererD3D11& renderer,unsigned scale) {
    constexpr uint32_t width=64,height=24;
    using Rgba=std::array<uint8_t,4>;
    const Rgba scene[]{{8,20,4,224},{96,128,160,128},{192,32,64,0},{24,12,36,255}};
    const Rgba masks[]{{16,12,2,255},{32,160,48,64},{64,16,96,48},{8,24,12,160}};
    WorldClear clear;clear.viewport={0,0,width,height};clear.targets={3951,0,0,0,0};clear.flags=1;
    for(unsigned quadrant=0;quadrant<4;++quadrant) {
        const int32_t x=int32_t((quadrant%2)*width/2),y=int32_t((quadrant/2)*height/2);
        clear.rectangle=std::array<int32_t,4>{x,y,x+int32_t(width/2),y+int32_t(height/2)};
        for(unsigned lane=0;lane<4;++lane)clear.color[lane]=scene[quadrant][lane]/255.f;
        renderer.clear(clear);
    }
    WorldResolve sceneCopy;sceneCopy.targets=clear.targets;sceneCopy.viewport=clear.viewport;
    sceneCopy.rectangle={0,0,width,height};sceneCopy.destination={3952,0x3952000,width,height,54,0};
    require(renderer.resolve(sceneCopy),"OW1 scene GPU resolve rejected");
    clear.targets={3953,0,0,0,0};clear.rectangle.reset();clear.color={1,0,1,1};renderer.clear(clear);
    for(unsigned quadrant=0;quadrant<4;++quadrant) {
        const int32_t x=int32_t(width/2+(quadrant%2)*width/4),y=int32_t(height/2+(quadrant/2)*height/4);
        clear.rectangle=std::array<int32_t,4>{x,y,x+int32_t(width/4),y+int32_t(height/4)};
        for(unsigned lane=0;lane<4;++lane)clear.color[lane]=masks[quadrant][lane]/255.f;
        renderer.clear(clear);
    }
    WorldResolve maskCopy=sceneCopy;maskCopy.targets=clear.targets;
    maskCopy.destination={3954,0x3954000,width,height,54,0};
    require(renderer.resolve(maskCopy),"OW1 lower-right mask atlas resolve rejected");
    auto draw=otherworldDraw(otherworldQuad(true),"WClientMod_OW1_2",3955);
    require(draw.textureMask==3,"OW1 second stage lost its scene/mask bindings");
    draw.textureObjects[0]=sceneCopy.destination;draw.textureObjects[1]=maskCopy.destination;
    clear.targets=draw.targets;clear.rectangle.reset();clear.color={.75f,.125f,.625f,.875f};renderer.clear(clear);
    clear.rectangle=std::array<int32_t,4>{0,0,int32_t(width/2),int32_t(height/2)};
    clear.color={1,0,1,1};renderer.clear(clear);
    require(renderer.draw(draw),"Original sewer scene-minus-mask composite rejected");
    const auto pixels=renderer.readSurface(draw.targets[0],false);
    require(pixels.size()==size_t(width)*height*scale*scale*8,"OW1 composite native output extent differs");
    unsigned checks=0,clamped=0,positiveAlpha=0;
    for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
        const unsigned quadrant=(y>=height*scale/2?2:0)+(x>=width*scale/2?1:0);
        uint16_t rgba[4];std::memcpy(rgba,pixels.data()+(size_t(y)*width*scale+x)*8,8);
        for(unsigned lane=0;lane<4;++lane) {
            const double expected=(std::max)(0,int(scene[quadrant][lane])-int(masks[quadrant][lane]))/255.0;
            const double actual=otherworldHalf(rgba[lane]);
            if(!std::isfinite(actual) || std::abs(actual-expected)>.002) {
                std::fprintf(stderr,"OtherworldComposite scale=%u pixel=%u,%u lane=%u actual=%g expected=%g\n",
                    scale,x,y,lane,actual,expected);
                require(false,"OW1 final composite lost full-screen coverage, atlas UVs, subtraction clamp or alpha");
            }
            ++checks;clamped+=expected==0;positiveAlpha+=lane==3 && expected>0;
        }
    }
    require(clamped>100*scale*scale && positiveAlpha>100*scale*scale,
        "OW1 subtraction fixture did not exercise clamping and positive alpha");
    std::printf("OtherworldComposite%u: %u full-output RGBA checks; poisoned half-view, resolved scene/atlas, subtraction clamp and alpha.\n",scale,checks);
}

// Independently reconstruct the view-space distance and four perturbed scene
// intensities. A stepped scene and varied noise make world coordinates, noise
// selection and both OffsetScale pairs observable; near/full/partial fades
// then feed the actual second stage through a resolved mask.
static void otherworldGrainContract(WorldRendererD3D11& renderer,unsigned scale) {
    constexpr uint32_t width=64,height=24;
    using Rgba=std::array<uint8_t,4>;
    const Rgba scene[]{{8,20,4,224},{96,128,160,128},{192,32,64,0},{24,12,36,255}};
    auto noise=std::make_shared<ColorImage>();noise->width=noise->height=4;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        const Rgba color{uint8_t(32+(x*41+y*29)%192),uint8_t(24+(x*53+y*37)%208),
            uint8_t(40+(x*17+y*61)%176),uint8_t(16+(x*71+y*23)%224)};
        noise->pixels.insert(noise->pixels.end(),color.begin(),color.end());
    }
    WorldClear clear;clear.viewport={0,0,width,height};clear.targets={3961,0,0,0,0};clear.flags=1;
    for(unsigned quadrant=0;quadrant<4;++quadrant) {
        const int32_t x=int32_t((quadrant%2)*width/2),y=int32_t((quadrant/2)*height/2);
        clear.rectangle=std::array<int32_t,4>{x,y,x+int32_t(width/2),y+int32_t(height/2)};
        for(unsigned lane=0;lane<4;++lane)clear.color[lane]=scene[quadrant][lane]/255.f;
        renderer.clear(clear);
    }
    WorldResolve sceneCopy;sceneCopy.targets=clear.targets;sceneCopy.viewport=clear.viewport;
    sceneCopy.rectangle={0,0,width,height};sceneCopy.destination={3962,0x3962000,width,height,54,0};
    require(renderer.resolve(sceneCopy),"OW1 grain scene GPU resolve rejected");
    auto geometry=otherworldQuad(false);
    auto grain=otherworldDraw(geometry,"WClientMod_OW1_1",3963);
    require(grain.textureMask==7,"OW1 grain lost scene/noise/depth bindings");
    grain.textureObjects[0]=sceneCopy.destination;grain.textures[1]=noise;
    grain.fragmentConstants[0]={1.f/width,1.f/height,1.f/width,1.f/height};
    grain.fragmentConstants[1]={.17f,0,0,1.2f};
    grain.fragmentConstants[2]={1,1000,.001f,0};
    grain.fragmentConstants[3]={2000,1001,2000,1998};
    grain.fragmentConstants[4]={0,0,1,1};
    grain.fragmentConstants[5]={.25f,0,3,75};
    grain.fragmentConstants[6]={0,.3f,0,50};
    grain.fragmentConstants[7]={0,0,20,0};
    auto composite=otherworldDraw(geometry,"WClientMod_OW1_2",3967);
    composite.textureObjects[0]=sceneCopy.destination;
    unsigned checks=0,visible=0,partial=0,suppressed=0;
    double maximumMask=0;
    for(float requestedDistance:{15.f,21.f,40.f}) {
        WorldClear depth;depth.targets[4]=3964;depth.viewport=clear.viewport;depth.flags=16;
        depth.depth=float((1000.0/requestedDistance-1)/999.0);renderer.clear(depth);
        const auto depthBytes=renderer.readSurface(3964,true);
        uint32_t packed=0;std::memcpy(&packed,depthBytes.data(),4);
        const double storedDepth=double(float(double(packed&0xffffff)/0xffffff));
        const double distance=1000/(1+999*storedDepth);
        WorldResolve depthCopy;depthCopy.targets=depth.targets;depthCopy.viewport=depth.viewport;
        depthCopy.flags=4;depthCopy.rectangle={0,0,width,height};depthCopy.destination={3965,0x3965000,width,height,23,0,1};
        require(renderer.resolve(depthCopy),"OW1 grain floating-depth resolve rejected");
        grain.textureObjects[2]=depthCopy.destination;
        clear.targets=grain.targets;clear.rectangle.reset();clear.color={1,0,1,0};renderer.clear(clear);
        require(renderer.draw(grain),"Original sewer world-position/depth grain rejected");
        const auto maskPixels=renderer.readSurface(grain.targets[0],false);
        require(maskPixels.size()==size_t(width)*height*scale*scale*8,"OW1 grain output extent differs");
        WorldResolve maskCopy=sceneCopy;maskCopy.targets=grain.targets;
        maskCopy.destination={3966,0x3966000,width,height,54,0};
        require(renderer.resolve(maskCopy),"OW1 grain mask GPU resolve rejected");
        composite.textureObjects[1]=maskCopy.destination;
        clear.targets=composite.targets;clear.color={1,0,1,1};renderer.clear(clear);
        require(renderer.draw(composite),"OW1 second stage rejected its original resolved grain mask");
        const auto finalPixels=renderer.readSurface(composite.targets[0],false);
        require(finalPixels.size()==maskPixels.size(),"OW1 two-stage output extent differs");
        const double fade=(std::clamp)((distance-16)*.1,0.0,1.0);
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const double u=(x+.5)/(width*scale),v=(y+.5)/(height*scale);
            bool stable=true;
            auto sampleNoise=[&](double nx,double ny) {
                for(double coordinate:{nx,ny})if(coordinate>0 && coordinate<1)
                    stable&=std::abs(coordinate*4-std::round(coordinate*4))>.0005;
                const unsigned ix=unsigned((std::clamp)(int(std::floor(nx*4)),0,3));
                const unsigned iy=unsigned((std::clamp)(int(std::floor(ny*4)),0,3));
                std::array<double,4> result{};
                for(unsigned lane=0;lane<4;++lane)result[lane]=noise->pixels[(iy*4+ix)*4+lane]/255.0;
                return result;
            };
            const double nx=.002*(.25*(2*u-1)*distance+3*distance+75);
            const double ny=.002*(.3*(2*v-1)*distance+50)+double(grain.fragmentConstants[1][0]);
            const double nz=.04*distance;
            const auto a=sampleNoise(nx,ny),b=sampleNoise(nx+.5,ny+.5);
            const auto c=sampleNoise(nz,ny),d=sampleNoise(nz-.5,ny-.5);
            double strength=0;
            constexpr double perturb[2][4]{{25,52,35,52},{25,12,15,12}};
            for(unsigned pair=0;pair<2;++pair)for(unsigned tap=0;tap<2;++tap) {
                const unsigned lane=tap*2;
                const double du=(pair?b[lane]+c[lane]:a[lane]+d[lane])-1;
                const double dv=(pair?b[lane+1]+c[lane+1]:a[lane+1]+d[lane+1])-1;
                const double su=u+du*perturb[pair][lane]/width;
                const double sv=v+dv*perturb[pair][lane+1]/height;
                stable&=std::abs(su-.5)>.0005 && std::abs(sv-.5)>.0005;
                const auto& texel=scene[(sv>=.5?2:0)+(su>=.5?1:0)];
                const double intensity=(texel[0]+2.0*texel[1]+texel[2])/(4*255.0);
                strength+=std::pow((std::clamp)(1-intensity*double(grain.fragmentConstants[1][3]),0.0,1.0),16);
            }
            if(!stable)continue; // Point-sample boundaries are sensitive to reciprocal rounding.
            const double expectedMask=.1*(std::min)(1.0,strength)*fade;
            const unsigned quadrant=(v>=.5?2:0)+(u>=.5?1:0);
            uint16_t mask[4],final[4];const size_t offset=(size_t(y)*width*scale+x)*8;
            std::memcpy(mask,maskPixels.data()+offset,8);std::memcpy(final,finalPixels.data()+offset,8);
            for(unsigned lane=0;lane<4;++lane) {
                const double maskExpected=lane==3?1:expectedMask;
                const double finalExpected=(std::max)(0.0,scene[quadrant][lane]/255.0-maskExpected);
                const double actualMask=otherworldHalf(mask[lane]),actualFinal=otherworldHalf(final[lane]);
                if(!std::isfinite(actualMask) || !std::isfinite(actualFinal) ||
                    std::abs(actualMask-maskExpected)>.003 || std::abs(actualFinal-finalExpected)>.003) {
                    std::fprintf(stderr,"OtherworldGrain scale=%u depth=%g pixel=%u,%u lane=%u mask=%g/%g final=%g/%g\n",
                        scale,distance,x,y,lane,actualMask,maskExpected,actualFinal,finalExpected);
                    require(false,"OW1 grain/composite differs from original world-noise, intensity power or distance fade");
                }
                checks+=2;
            }
            visible+=expectedMask>.01;partial+=fade>0 && fade<1 && expectedMask>.005;
            suppressed+=fade==0;maximumMask=(std::max)(maximumMask,expectedMask);
        }
    }
    require(visible>100*scale*scale && partial>100*scale*scale && suppressed>100*scale*scale && maximumMask>.08,
        "OW1 grain fixture failed to exercise visible noise and suppressed/partial/full distance fades");
    std::printf("OtherworldGrain%u: %u mask/final RGBA oracle checks; world-position noise, Xenon depth, near/partial/full fades and resolved two-stage alpha.\n",scale,checks);
}
static void otherworldContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        otherworldCompositeContract(renderer,scale);
        otherworldGrainContract(renderer,scale);
        context->ClearState();
    }
}
