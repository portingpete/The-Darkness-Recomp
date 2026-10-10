// Model the shipped bloom filters independently on the guest-sized image.
// Fixtures are first drawn and resolved at physical resolution, so these
// contracts exercise the render-target path rather than a CPU upload shortcut.
using BloomPixel=std::array<double,4>;
struct BloomImage {
    unsigned width=0,height=0;
    std::vector<BloomPixel> pixels;
};
static BloomImage bloomImage(const ColorImage& image) {
    BloomImage result{image.width,image.height,{}};
    result.pixels.resize(size_t(image.width)*image.height);
    for(size_t n=0;n<result.pixels.size();++n)for(unsigned c=0;c<4;++c)
        result.pixels[n][c]=image.pixels[n*4+c]/255.0;
    return result;
}
static BloomImage bloomAverage(const BloomImage& physical,unsigned scale) {
    BloomImage logical{physical.width/scale,physical.height/scale,{}};
    logical.pixels.resize(size_t(logical.width)*logical.height);
    for(unsigned y=0;y<logical.height;++y)for(unsigned x=0;x<logical.width;++x)
        for(unsigned dy=0;dy<scale;++dy)for(unsigned dx=0;dx<scale;++dx)for(unsigned c=0;c<4;++c)
            logical.pixels[size_t(y)*logical.width+x][c]+=
                physical.pixels[size_t(y*scale+dy)*physical.width+x*scale+dx][c]/(scale*scale);
    return logical;
}
static BloomPixel bloomSample(const BloomImage& image,double u,double v,bool linear=true) {
    auto at=[&](int x,int y)->const BloomPixel& {
        x=std::clamp(x,0,int(image.width)-1);y=std::clamp(y,0,int(image.height)-1);
        return image.pixels[size_t(y)*image.width+x];
    };
    if(!linear)return at(int(std::floor(u*image.width)),int(std::floor(v*image.height)));
    const double px=u*image.width-.5,py=v*image.height-.5;
    const int x=int(std::floor(px)),y=int(std::floor(py));
    const double fx=px-x,fy=py-y;
    BloomPixel result{};
    for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)for(unsigned c=0;c<4;++c)
        result[c]+=at(x+dx,y+dy)[c]*(dx?fx:1-fx)*(dy?fy:1-fy);
    return result;
}
static std::shared_ptr<ColorImage> bloomFixture(unsigned width,unsigned height,unsigned scale,
                                               unsigned kind,unsigned phase=0) {
    auto image=std::make_shared<ColorImage>();image->width=width*scale;image->height=height*scale;
    image->authoredMips=true;image->pixels.resize(size_t(image->width)*image->height*4);
    for(unsigned y=0;y<image->height;++y)for(unsigned x=0;x<image->width;++x) {
        auto* p=image->pixels.data()+(size_t(y)*image->width+x)*4;
        if(kind==0) {
            // Move one-pixel lines through all phases of an eight-texel cell.
            p[0]=x%(8*scale)==phase?255:0;p[1]=y%(8*scale)==phase?255:0;
            p[2]=(x+y)%(8*scale)==phase?255:0;
        } else if(kind==1) {
            p[0]=(x+y)%2?255:0;p[1]=x%2?255:0;p[2]=y%2?255:0;
        } else if(kind==2) {
            const bool impulse=x%(8*scale)==3*scale+phase%scale &&
                y%(8*scale)==3*scale+phase/scale;
            p[0]=p[1]=p[2]=impulse?255:0;
        } else {
            // Fine structure within every guest pixel, plus a broad bright band.
            p[0]=uint8_t((x*71+y*37+phase*11)%256);
            p[1]=uint8_t((x*19+y*97+phase*23)%256);
            p[2]=uint8_t((x/scale>width/3 && x/scale<width*2/3)?224:(x+y)%2?96:16);
        }
        p[3]=255;
    }
    return image;
}
static WorldTexture bloomSeed(WorldRendererD3D11& renderer,const std::shared_ptr<ColorImage>& image,
                              unsigned scale,uint32_t target,uint32_t object,uint32_t format=6,int exponent=0) {
    const unsigned width=image->width/scale,height=image->height/scale;
    auto draw=colorGradeDraw(colorGradeQuad(width,height,width,height),target,width,height);
    draw.fragmentName="XREngine_CCFuser";draw.fragmentConstants[0]={1,1,1,1};
    draw.textures[1]=image;draw.samplers[1]=colorGradeSampler(false,2);
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
    renderer.clear(clear);require(renderer.draw(draw),"Bloom physical source draw rejected");
    return colorGradeResolve(renderer,draw,object,width,height,format,exponent);
}
static void bloomCheckSurface(WorldRendererD3D11& renderer,const WorldDraw& draw,unsigned scale,
                              const std::vector<BloomPixel>& expected,const char* name,double tolerance=.003) {
    const auto actual=renderer.readSurface(draw.targets[0],false);
    const unsigned width=draw.viewport[2]*scale,height=draw.viewport[3]*scale;
    require(actual.size()==size_t(width)*height*8 && expected.size()==size_t(width)*height,
            "Bloom filter physical extent differs");
    unsigned mismatches=0;double maximum=0;
    for(size_t n=0;n<expected.size();++n)for(unsigned c=0;c<4;++c) {
        uint16_t raw=0;std::memcpy(&raw,actual.data()+n*8+c*2,2);
        const double delta=std::abs(materialHalf(raw)-expected[n][c]);
        maximum=(std::max)(maximum,delta);
        if(!std::isfinite(delta) || delta>tolerance) {
            if(mismatches++==0)std::fprintf(stderr,"Bloom mismatch %s scale=%u at %zu,%zu lane%u actual=%g expected=%g\n",
                name,scale,n%width,n/width,c,materialHalf(raw),expected[n][c]);
        }
    }
    std::printf("BloomResolution: %s scale=%u maxDelta=%.6f mismatches=%u\n",name,scale,maximum,mismatches);
    require(mismatches==0,"Bloom differs from independent guest-resolution filter oracle");
}
static void bloomDraw(WorldRendererD3D11& renderer,const WorldDraw& draw) {
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
    renderer.clear(clear);require(renderer.draw(draw),"Bloom filter draw rejected");
}
static BloomPixel bloomShrinkOracle(const BloomImage& source,double u,double v,bool linear=true) {
    BloomPixel result{};
    for(int dy:{-3,-1,1,3})for(int dx:{-3,-1,1,3}) {
        const auto tap=bloomSample(source,u+double(dx)/64,v+double(dy)/64,linear);
        for(unsigned c=0;c<4;++c)result[c]+=tap[c]/16;
    }
    return result;
}
static void bloomShrinkContract(WorldRendererD3D11& renderer,unsigned scale) {
    auto draw=colorGradeDraw(colorGradeQuad(8,8,8,8),4202,8,8);
    draw.fragmentName="XRUtil_ShrinkTexture8";draw.fragmentConstants[0]={1.0f/64,1.0f/64,0,0};
    draw.samplers[0]=colorGradeSampler(true,2);
    unsigned cases=0;
    for(unsigned kind=0;kind<3;++kind) {
        const unsigned phases=kind==0?8*scale:kind==2?scale*scale:1;
        for(unsigned phase=0;phase<phases;++phase) {
            const auto image=bloomFixture(64,64,scale,kind,phase);
            const auto source=bloomAverage(bloomImage(*image),scale);
            draw.textureObjects[0]=bloomSeed(renderer,image,scale,4201,4301);
            bloomDraw(renderer,draw);
            std::vector<BloomPixel> expected(size_t(8*scale)*8*scale);
            for(unsigned y=0;y<8*scale;++y)for(unsigned x=0;x<8*scale;++x)
                expected[size_t(y)*8*scale+x]=bloomShrinkOracle(source,(x/scale+.5)/8,(y/scale+.5)/8);
            const std::string name="Shrink8-kind"+std::to_string(kind)+"-phase"+std::to_string(phase);
            bloomCheckSurface(renderer,draw,scale,expected,name.c_str(),.0003);++cases;
        }
    }
    std::printf("BloomShrink%u: %u checker, thin-line and fine-impulse phases; original sixteen bilinear taps retained.\n",scale,cases);
}
static std::array<float,9> bloomWeights() {
    std::array<float,9> weights{};double total=0;
    for(unsigned i=0;i<weights.size();++i) {weights[i]=float(std::exp(-double(i*i)/32));total+=weights[i]*(i?2:1);}
    for(auto& value:weights)value=float(value/total);
    return weights;
}
static WorldDraw bloomGaussianDraw(unsigned width,unsigned height,const char* program) {
    auto draw=colorGradeDraw(colorGradeQuad(width,height,width,height),4203,width,height);
    draw.fragmentName=program;draw.samplers[0]=colorGradeSampler(true,2);
    draw.fragmentConstants[0]={.01f,.02f,.03f,1};draw.fragmentConstants[1]={.8f,.9f,1,1};
    draw.fragmentConstants[2]={1.15f,.9f,1.05f,0};
    draw.fragmentConstants[3]={.5f/width,.5f/height,.5f/width,.5f/height};
    draw.fragmentConstants[4]={1-.5f/width,1-.5f/height,1-.5f/width,1-.5f/height};
    const auto weights=bloomWeights();draw.fragmentConstants[5]={0,0,0,weights[0]};
    for(unsigned i=0;i<4;++i) {
        draw.fragmentConstants[6][i]=weights[i+1];draw.fragmentConstants[7][i]=weights[i+5];
        const float offsetA=float(1.1+2.4*i)/width,offsetB=float(2.3+2.4*i)/width;
        draw.fragmentConstants[8+i]={offsetA,0,offsetB,0};
    }
    return draw;
}
static BloomPixel bloomGaussianOracle(const BloomImage& source,const WorldDraw& draw,double u,double v,bool linear=true) {
    const auto center=bloomSample(source,u,v,linear);BloomPixel result{};
    for(unsigned c=0;c<4;++c)result[c]=center[c]*draw.fragmentConstants[5][3];
    for(unsigned pair=0;pair<4;++pair)for(unsigned lane=0;lane<2;++lane)for(int sign:{-1,1}) {
        const auto& offset=draw.fragmentConstants[8+pair];
        const double tu=std::clamp(u+sign*offset[lane*2],double(draw.fragmentConstants[3][0]),double(draw.fragmentConstants[4][0]));
        const double tv=std::clamp(v+sign*offset[lane*2+1],double(draw.fragmentConstants[3][1]),double(draw.fragmentConstants[4][1]));
        const auto tap=bloomSample(source,tu,tv,linear);
        const unsigned n=pair*2+lane;
        const float weight=draw.fragmentConstants[n<4?6:7][n%4];
        for(unsigned c=0;c<3;++c)result[c]+=tap[c]*weight;
    }
    for(unsigned c=0;c<3;++c)result[c]=std::pow(std::clamp(result[c]*draw.fragmentConstants[1][c]+draw.fragmentConstants[0][c],0.0,1.0),draw.fragmentConstants[2][c]);
    if(draw.fragmentName=="XREngine_GaussClampedHurt") {
        const double nx=(u-draw.fragmentConstants[3][0])/(draw.fragmentConstants[4][0]-draw.fragmentConstants[3][0]);
        const double ny=(v-draw.fragmentConstants[3][1])/(draw.fragmentConstants[4][1]-draw.fragmentConstants[3][1]);
        const double radius=std::hypot(.5-nx,.5-ny);
        const double fade=std::clamp(radius*draw.fragmentConstants[1][3]+(1-radius)*draw.fragmentConstants[0][3],0.0,1.0);
        for(unsigned c=0;c<4;++c)result[c]=fade*result[c]+(1-fade)*center[c];
    }
    return result;
}
static void bloomGaussianContract(WorldRendererD3D11& renderer,unsigned scale) {
    constexpr unsigned width=32,height=24;
    const auto physical=bloomFixture(width,height,scale,3),cpu=bloomFixture(width,height,1,3);
    const auto native=bloomImage(*physical),logical=bloomAverage(native,scale),cpuSource=bloomImage(*cpu);
    const auto resolved=bloomSeed(renderer,physical,scale,4201,4301);
    for(const char* program:{"XREngine_GaussClamped","XREngine_GaussClampedHurt"})for(unsigned mode=0;mode<3;++mode) {
        auto draw=bloomGaussianDraw(width,height,program);
        // Hurt's unblurred component retains physical detail and its original fade.
        const bool hurt=draw.fragmentName=="XREngine_GaussClampedHurt";
        if(hurt) {draw.fragmentConstants[0][3]=.2f;draw.fragmentConstants[1][3]=1.4f;}
        if(mode==1)draw.textures[0]=cpu;
        else draw.textureObjects[0]=resolved;
        if(mode==2) {
            draw.samplers[0]=colorGradeSampler(false,2);
            // Point sampling at a half guest texel is an exact physical texel
            // boundary at even scales. Avoid implementation-dependent ties so
            // this control checks preserved filtering rather than tie breaking.
            draw.fragmentConstants[3]={0,0,0,0};draw.fragmentConstants[4]={1,1,1,1};
            for(unsigned pair=0;pair<4;++pair)
                draw.fragmentConstants[8+pair]={float(.93+1.17*(pair*2))/width,0,
                    float(.93+1.17*(pair*2+1))/width,0};
        }
        bloomDraw(renderer,draw);
        std::vector<BloomPixel> expected(size_t(width*scale)*height*scale);
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const bool logicalFilter=mode==0&&!hurt;
            const double u=logicalFilter?(x/scale+.5)/width:(x+.5)/(width*scale);
            const double v=logicalFilter?(y/scale+.5)/height:(y+.5)/(height*scale);
            expected[size_t(y)*width*scale+x]=bloomGaussianOracle(logicalFilter?logical:mode==1?cpuSource:native,draw,u,v,mode!=2);
        }
        const std::string name=std::string(program)+(mode==0?"-resolved-linear":mode==1?"-CPU-linear":"-resolved-point");
        bloomCheckSurface(renderer,draw,scale,expected,name.c_str(),mode==2?.004:.006);
    }
}
static void bloomWideContract(WorldRendererD3D11& renderer,unsigned scale) {
    // Retail bloom resolves use 16-bit normalized storage plus a guest exponent.
    // Keep that decoding after the logical proxy is selected.
    for(bool gaussian:{false,true}) {
        const unsigned width=gaussian?32:64,height=gaussian?24:64;
        const auto image=bloomFixture(width,height,scale,3);
        const uint32_t seedTarget=gaussian?4207:4206;
        const auto resolved=bloomSeed(renderer,image,scale,seedTarget,4301,26,4);
        // Read the real FP16 seed (and verify it against the authored bytes),
        // then independently model the resolve's divide16, UNORM16 rounding
        // and multiply16. That storage quantization is separate from filtering.
        auto decoded=bloomImage(*image);
        const auto seed=renderer.readSurface(seedTarget,false);
        require(seed.size()==decoded.pixels.size()*8,"Wide bloom seed extent differs");
        for(size_t n=0;n<decoded.pixels.size();++n)for(unsigned c=0;c<4;++c) {
            uint16_t raw=0;std::memcpy(&raw,seed.data()+n*8+c*2,2);
            const double half=materialHalf(raw);
            require(std::abs(half-decoded.pixels[n][c])<.0006,"Wide bloom physical seed differs from authored pixels");
            const double normalized=std::clamp(half/16,0.0,1.0);
            decoded.pixels[n][c]=std::round(normalized*65535)/65535*16;
        }
        const auto logical=bloomAverage(decoded,scale);
        auto draw=gaussian?bloomGaussianDraw(width,height,"XREngine_GaussClamped"):
            colorGradeDraw(colorGradeQuad(8,8,8,8),4202,8,8);
        if(!gaussian) {
            draw.fragmentName="XRUtil_ShrinkTexture8";
            draw.fragmentConstants[0]={1.0f/64,1.0f/64,0,0};
            draw.samplers[0]=colorGradeSampler(true,2);
        }
        draw.textureObjects[0]=resolved;bloomDraw(renderer,draw);
        const unsigned outputWidth=draw.viewport[2],outputHeight=draw.viewport[3];
        std::vector<BloomPixel> expected(size_t(outputWidth*scale)*outputHeight*scale);
        for(unsigned y=0;y<outputHeight*scale;++y)for(unsigned x=0;x<outputWidth*scale;++x) {
            const double u=(x/scale+.5)/outputWidth,v=(y/scale+.5)/outputHeight;
            expected[size_t(y)*outputWidth*scale+x]=gaussian?
                bloomGaussianOracle(logical,draw,u,v):bloomShrinkOracle(logical,u,v);
        }
        // FP16 target conversion can discard one full mantissa step (.000488
        // around .5); allow that final storage step after decoding the input.
        bloomCheckSurface(renderer,draw,scale,expected,
            gaussian?"GaussClamped-UNORM16-exponent4":"Shrink8-UNORM16-exponent4",gaussian?.006:.0006);
    }
}
static void bloomFinalContract(WorldRendererD3D11& renderer,unsigned scale) {
    constexpr unsigned width=64,height=48;
    const auto physical=bloomFixture(32,24,scale,3),scene=bloomFixture(width,height,scale,3,7);
    const auto logical=bloomAverage(bloomImage(*physical),scale),nativeScene=bloomImage(*scene);
    const auto resolved=bloomSeed(renderer,physical,scale,4201,4301);
    const auto sceneResolved=bloomSeed(renderer,scene,scale,4204,4304);
    for(unsigned flags:{0u,8u,10u}) {
        auto draw=colorGradeDraw(colorGradeQuad(width,height,width,height),4205,width,height);
        draw.options.modes[1]=0;draw.options.coordinates[1]=0;
        draw.fragmentName="XREngine_Final5";draw.fragmentFlags=flags;
        draw.textureObjects[0]=sceneResolved;draw.samplers[0]=colorGradeSampler(false,2);
        if(flags&8) {draw.textureObjects[1]=resolved;draw.samplers[1]=colorGradeSampler(true,2);}
        draw.fragmentConstants[0]={.9f,1.1f,.8f,1};draw.fragmentConstants[1]={0,0,1,1};
        bloomDraw(renderer,draw);std::vector<BloomPixel> expected(size_t(width*scale)*height*scale);
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const auto bloom=bloomSample(logical,(x+.5)/(width*scale),(y+.5)/(height*scale));
            auto& result=expected[size_t(y)*width*scale+x];result=nativeScene.pixels[size_t(y)*width*scale+x];
            for(unsigned c=0;c<3;++c) {
                double value=result[c]*result[c];
                if(flags&8)value+=bloom[c]*bloom[c]*(1-value);
                if(flags&2)value=1-std::exp(-(std::max)(value*draw.fragmentConstants[0][c],1e-7));
                result[c]=std::sqrt((std::max)(value,1e-8));
            }
        }
        const std::string name="Final5-flags"+std::to_string(flags)+"-scene-physical-bloom-logical";
        bloomCheckSurface(renderer,draw,scale,expected,name.c_str(),.006);
    }
}
// The main retail glow lives in a 160x90 corner of a 1280x720 atlas, with
// HDR scene radiance, two resolves of the same bloom object and a negative
// vertical bias. A source capped at one produces no halo for this fixture.
static std::shared_ptr<StoredGeometry> bloomRetailQuad(bool final) {
    auto original=colorGradeQuad(final?1280:160,final?720:90,1280,720);
    constexpr float atlasUvs[4][2]{{0,.125f},{0,0},{.125f,0},{.125f,.125f}};
    if(!final) {
        for(unsigned v=0;v<4;++v)for(unsigned c=0;c<2;++c)
            put(original->vertices.data()+v*20+12+c*4,std::bit_cast<uint32_t>(atlasUvs[v][c]));
        return original;
    }
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=28;
    geometry->formats[0]=3;geometry->formats[1]=geometry->formats[2]=2;
    geometry->vertices.resize(112);geometry->indices=original->indices;
    for(unsigned v=0;v<4;++v) {
        auto* target=geometry->vertices.data()+v*28;
        std::memcpy(target,original->vertices.data()+v*20,20);
        for(unsigned c=0;c<2;++c)put(target+20+c*4,std::bit_cast<uint32_t>(atlasUvs[v][c]));
    }
    return geometry;
}
static WorldTexture bloomRetailResolve(WorldRendererD3D11& renderer,const WorldDraw& draw,
                                       uint32_t object,bool atlas) {
    WorldResolve resolve;resolve.targets=draw.targets;resolve.viewport=draw.viewport;
    resolve.rectangle={0,0,atlas?160u:1280u,atlas?96u:720u};
    resolve.destination={object,object*4096u,1280,720,26,4};resolve.exponent=-4;
    require(renderer.resolve(resolve),"Retail HDR bloom resolve rejected");return resolve.destination;
}
static double bloomRetailStored(double value) {
    // Independently model FP16 target storage followed by the retail /16
    // UNORM16 resolve and +4 texture fetch exponent. GPU rounding may differ
    // by one final mantissa step, covered separately by comparison tolerances.
    if(value>0) {
        int exponent=0;std::frexp(value,&exponent);
        const double step=std::ldexp(1.0,(std::max)(exponent-11,-24));
        value=std::nearbyint(value/step)*step;
    }
    return std::round(std::clamp(value/16,0.0,1.0)*65535)*16/65535;
}
static BloomPixel bloomRetailScene(double u,double v) {
    const double px=u*1280-.5,py=v*720-.5;
    const int x=int(std::floor(px)),y=int(std::floor(py));
    const double fx=px-x,fy=py-y;double radiance=0;
    for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx) {
        const int sx=std::clamp(x+int(dx),0,1279),sy=std::clamp(y+int(dy),0,719);
        if(sx>=624 && sx<656 && sy>=344 && sy<376)
            radiance+=16*(dx?fx:1-fx)*(dy?fy:1-fy);
    }
    return {radiance,radiance,radiance,1};
}
static WorldDraw bloomRetailGaussian(bool vertical) {
    auto draw=colorGradeDraw(bloomRetailQuad(false),4802,1280,720);
    draw.fragmentName="XREngine_GaussClamped";draw.samplers[0]=colorGradeSampler(true,2);
    draw.fragmentConstants[0]=vertical?EngineVector{-.1f,-.1f,-.1f,1}:EngineVector{0,0,0,1};
    draw.fragmentConstants[1]={.15060241520404816f,.15060241520404816f,.15060241520404816f,1};
    draw.fragmentConstants[2]={1,1,1,vertical?0.0f:1.0f};
    draw.fragmentConstants[3]={.5f/1280,.5f/720,.5f/1280,.5f/720};
    draw.fragmentConstants[4]={159.5f/1280,89.5f/720,159.5f/1280,89.5f/720};
    draw.fragmentConstants[5]={.0625f,.0625f,0,.5f};
    draw.fragmentConstants[6]={.8562499284744263f,.6812500357627869f,.5262500047683716f,.39124998450279236f};
    draw.fragmentConstants[7]={.2762500047683716f,.1812499761581421f,.10624998807907104f,.051249995827674866f};
    constexpr std::array<float,8> horizontal{.0011507755843922496f,.0027107223868370056f,
        .004269967321306467f,.005828175228089094f,.007384757045656443f,.008938577957451344f,
        .010487132705748081f,.012023627758026123f};
    constexpr std::array<float,8> upright{.002045823261141777f,.004819062072783709f,
        .007591052912175655f,.01036120019853115f,.01312845665961504f,.015890805050730705f,
        .01864379085600376f,.021375339478254318f};
    for(unsigned pair=0;pair<4;++pair)draw.fragmentConstants[8+pair]=vertical?
        EngineVector{0,upright[pair*2],0,upright[pair*2+1]}:
        EngineVector{horizontal[pair*2],0,horizontal[pair*2+1],0};
    return draw;
}
static BloomImage bloomRetailGaussianOracle(const BloomImage& source,const WorldDraw& captured) {
    // Convert the captured full-atlas UVs into coordinates of the active
    // 160x90 CPU image; keep the original seventeen taps and bias unchanged.
    auto local=captured;
    for(unsigned n:{3u,4u,8u,9u,10u,11u})for(auto& lane:local.fragmentConstants[n])lane*=8;
    BloomImage result{160,90,{}};result.pixels.resize(160*90);
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x)
        result.pixels[size_t(y)*160+x]=bloomGaussianOracle(source,local,(x+.5)/160,(y+.5)/90);
    return result;
}
static BloomImage bloomRetailCheck(WorldRendererD3D11& renderer,const WorldDraw& draw,unsigned scale,
                                   BloomImage expected,const char* name,double tolerance) {
    const auto pixels=renderer.readSurface(draw.targets[0],false);
    const unsigned width=1280*scale;
    require(pixels.size()==size_t(width)*720*scale*8,"Retail bloom atlas extent differs");
    unsigned mismatches=0;double maximum=0;
    for(unsigned y=0;y<90*scale;++y)for(unsigned x=0;x<160*scale;++x)for(unsigned c=0;c<3;++c) {
        uint16_t raw=0;std::memcpy(&raw,pixels.data()+(size_t(y)*width+x)*8+c*2,2);
        const double wanted=expected.pixels[size_t(y/scale)*160+x/scale][c];
        const double delta=std::abs(materialHalf(raw)-wanted);maximum=(std::max)(maximum,delta);
        if(!std::isfinite(delta) || delta>tolerance) {
            if(mismatches++==0)std::fprintf(stderr,"Retail bloom mismatch %s scale=%u at %u,%u lane%u actual=%g expected=%g\n",
                name,scale,x,y,c,materialHalf(raw),wanted);
        }
    }
    std::printf("BloomRetail: %s scale=%u maxDelta=%.6f mismatches=%u\n",name,scale,maximum,mismatches);
    require(mismatches==0,"Retail bloom atlas differs from original HDR filter oracle");
    for(auto& pixel:expected.pixels)for(auto& lane:pixel)lane=bloomRetailStored(lane);
    return expected;
}
static void bloomRetailChainContract(WorldRendererD3D11& renderer,unsigned scale) {
    auto image=std::make_shared<ColorImage>();image->width=1280;image->height=720;image->authoredMips=true;
    image->pixels.resize(size_t(1280)*720*4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        auto* pixel=image->pixels.data()+(size_t(y)*1280+x)*4;
        if(x>=624 && x<656 && y>=344 && y<376)pixel[0]=pixel[1]=pixel[2]=255;
        pixel[3]=255;
    }
    auto seed=colorGradeDraw(colorGradeQuad(1280,720,1280,720),4801,1280,720);
    seed.fragmentName="XREngine_CCFuser";seed.fragmentConstants[0]={16,16,16,1};
    seed.textures[1]=image;seed.samplers[1]=colorGradeSampler(false,2);bloomDraw(renderer,seed);
    {
        const auto pixels=renderer.readSurface(4801,false);std::array<double,3> sum{},peak{};
        require(pixels.size()==size_t(1280*scale)*720*scale*8,"Retail HDR scene extent differs");
        for(size_t n=0;n<pixels.size()/8;++n)for(unsigned c=0;c<3;++c) {
            uint16_t raw=0;std::memcpy(&raw,pixels.data()+n*8+c*2,2);
            const double value=materialHalf(raw);sum[c]+=value;peak[c]=(std::max)(peak[c],value);
        }
        for(unsigned c=0;c<3;++c)require(peak[c]>15.99 && std::abs(sum[c]/(scale*scale)-16384)<.01,
            "Retail HDR scene lost peak radiance or integrated light energy");
    }
    const auto scene=bloomRetailResolve(renderer,seed,4901,false);
    auto shrink=colorGradeDraw(colorGradeQuad(160,90,1280,720),4802,1280,720);
    shrink.fragmentName="XRUtil_ShrinkTexture8";shrink.fragmentConstants[0]={1.0f/1280,1.0f/720,0,0};
    shrink.textureObjects[0]=scene;shrink.samplers[0]=colorGradeSampler(true,2);
    // Bright magenta outside the active atlas, including the six copied pad
    // rows, makes an incorrect UV clamp visible instead of hiding it in black.
    WorldClear clear;clear.targets=shrink.targets;clear.viewport=shrink.viewport;
    clear.flags=1;clear.color={16,0,16,1};renderer.clear(clear);
    bloomRetailResolve(renderer,shrink,4902,false);
    require(renderer.draw(shrink),"Retail 8x HDR shrink rejected");
    BloomImage reduced{160,90,{}};reduced.pixels.resize(160*90);
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x) {
        auto& value=reduced.pixels[size_t(y)*160+x];
        for(int dy:{-3,-1,1,3})for(int dx:{-3,-1,1,3}) {
            const auto tap=bloomRetailScene((x+.5)/160+double(dx)/1280,(y+.5)/90+double(dy)/720);
            for(unsigned c=0;c<4;++c)value[c]+=tap[c]/16;
        }
    }
    reduced=bloomRetailCheck(renderer,shrink,scale,std::move(reduced),"HDR-shrink-atlas",.002);
    auto atlas=bloomRetailResolve(renderer,shrink,4902,true);
    for(bool vertical:{false,true}) {
        auto gaussian=bloomRetailGaussian(vertical);gaussian.textureObjects[0]=atlas;
        renderer.clear(clear);require(renderer.draw(gaussian),"Retail HDR Gaussian rejected");
        reduced=bloomRetailCheck(renderer,gaussian,scale,bloomRetailGaussianOracle(reduced,gaussian),
            vertical?"negative-bias-vertical":"horizontal",vertical?.004:.008);
        atlas=bloomRetailResolve(renderer,gaussian,4902,true);
    }
    double bloomPeak=0;for(const auto& pixel:reduced.pixels)bloomPeak=(std::max)(bloomPeak,pixel[0]);
    require(bloomPeak>.16 && bloomPeak<.18,"Retail negative-bias HDR oracle lost its expected halo");
    auto lut=colorGradeDraw(colorGradeQuad(324,18,1280,720),4803,1280,720);
    const auto identity=colorGradeCube(false);lut.fragmentName="XREngine_CCFuser";
    lut.fragmentConstants[0]={1,1,1,1};lut.textures[1]=identity;lut.samplers[1]=colorGradeSampler(false,2);
    bloomDraw(renderer,lut);const auto lookup=colorGradeResolve(renderer,lut,4903,324,18,26,4);
    auto final=colorGradeDraw(bloomRetailQuad(true),4804,1280,720);
    final.options.modes[1]=0;final.options.coordinates[1]=1;
    final.fragmentName="XREngine_Final5";final.fragmentFlags=14;
    final.textureObjects[0]=scene;final.textureObjects[1]=atlas;final.textureObjects[2]=lookup;
    for(unsigned slot=0;slot<3;++slot)final.samplers[slot]=colorGradeSampler(true,2);
    final.fragmentConstants[0]={.4287094f,.4287094f,.5278032f,.25f};
    final.fragmentConstants[1]={.5f/1280,.5f/720,159.5f/1280,89.5f/720};bloomDraw(renderer,final);
    const auto pixels=renderer.readSurface(4804,false);const unsigned width=1280*scale,height=720*scale;
    require(pixels.size()==size_t(width)*height*8,"Retail Final5 physical extent differs");
    unsigned mismatches=0,comparisons=0;double maximum=0;
    auto actual=[&](unsigned x,unsigned y,unsigned c) {
        uint16_t raw=0;std::memcpy(&raw,pixels.data()+(size_t(y)*width+x)*8+c*2,2);return double(materialHalf(raw));
    };
    auto compare=[&](unsigned x,unsigned y) {
        const double u=(x+.5)/width,v=(y+.5)/height;
        const auto glow=bloomSample(reduced,u,v);
        const double sceneValue=x/scale>=624 && x/scale<656 && y/scale>=344 && y/scale<376?16:0;
        for(unsigned c=0;c<3;++c) {
            const double sceneSquared=sceneValue*sceneValue,glowSquared=glow[c]*glow[c];
            const double combined=sceneSquared+glowSquared-std::clamp(sceneSquared*glowSquared,0.0,1.0);
            const double transfer=std::sqrt((std::max)(1-std::exp(-(std::max)(combined*final.fragmentConstants[0][c],1e-7)),1e-8));
            // An authored identity cube is separable: trilinear interpolation
            // reduces to interpolation of the two stored values in this lane.
            const double position=std::clamp(transfer,0.0,1.0)*17;
            const unsigned lo=unsigned(std::floor(position)),hi=(std::min)(lo+1,17u);
            const double fraction=position-lo;
            const double wanted=bloomRetailStored(lo/17.0)*(1-fraction)+bloomRetailStored(hi/17.0)*fraction;
            const double delta=std::abs(actual(x,y,c)-wanted);maximum=(std::max)(maximum,delta);++comparisons;
            if(!std::isfinite(delta) || delta>.004) {
                if(mismatches++==0)std::fprintf(stderr,"Retail Final5 mismatch scale=%u at %u,%u lane%u actual=%g expected=%g\n",
                    scale,x,y,c,actual(x,y,c),wanted);
            }
        }
    };
    // Bound CPU work to the emitter and its full halo; sample the physical
    // image edges too, where the atlas sentinel exposes missing GlowClamp.
    for(unsigned y=248*scale;y<472*scale;y+=2)for(unsigned x=480*scale;x<800*scale;x+=2)compare(x,y);
    for(unsigned x=0;x<width;x+=16*scale){compare(x,0);compare(x,height-1);}
    for(unsigned y=0;y<height;y+=16*scale){compare(0,y);compare(width-1,y);}
    require(actual(672*scale,360*scale,0)>.10 && actual(640*scale,456*scale,0)<.002,
        "Retail HDR emitter did not produce a broad halo against a dark background");
    std::printf("BloomRetail: Final5-flags14 scale=%u halo=%g maxDelta=%.6f comparisons=%u mismatches=%u\n",
        scale,actual(672*scale,360*scale,0),maximum,comparisons,mismatches);
    require(mismatches==0,"Retail Final5 halo differs from independent HDR/atlas/LUT oracle");
}
static void bloomResolutionContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        bloomShrinkContract(renderer,scale);bloomGaussianContract(renderer,scale);
        bloomWideContract(renderer,scale);bloomFinalContract(renderer,scale);bloomRetailChainContract(renderer,scale);
    }
    context->ClearState();
    std::puts("BloomResolution passed: RGBA8/UNORM16-exponent4 sources; 8x shrink fine-pixel phases; original Gaussian; retail radiance16 atlas/negative-bias/Final14 halo; physical scene, hurt, CPU and point sources at scales1/2/3.");
}
