#include "runtime/native/graphics_settings.h"

// Exercise the material calibration through the renderer and the captured
// retail HDR/atlas/negative-bias/Final5 chain. The decoded BC3-style fixture has
// a sparse RGB core and a wider, lower alpha mask, as the arm signs do.
static WorldDraw darknessArmBloomDraw(uint16_t textureId) {
    auto image=std::make_shared<ColorImage>();image->width=image->height=512;
    image->authoredMips=true;image->pixels.resize(size_t(512)*512*4);
    for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x) {
        auto* pixel=image->pixels.data()+(size_t(y)*512+x)*4;
        if(x>=128 && x<384 && y>=128 && y<384)pixel[0]=pixel[1]=pixel[2]=255;
        pixel[3]=x>=64 && x<448 && y>=64 && y<448?112:32;
    }
    auto geometry=colorGradeQuad(64,64,1280,720);
    for(unsigned v=0;v<4;++v) {
        auto* position=geometry->vertices.data()+v*20;
        const auto get=[&](unsigned lane) {
            uint32_t bits=0;for(unsigned n=0;n<4;++n)bits=(bits<<8)|position[lane*4+n];
            return std::bit_cast<float>(bits);
        };
        put(position,std::bit_cast<uint32_t>(get(0)+2.0f*608/1280));
        put(position+4,std::bit_cast<uint32_t>(get(1)-2.0f*328/720));
    }
    auto draw=colorGradeDraw(geometry,6801,1280,720);
    draw.fragmentName="XRUtil_RenderSurface";draw.textureMask=1;
    draw.fragmentConstants[0]={4,2,1,.5f};
    draw.textureIds[0]=textureId;draw.textureObjects[0]={textureId,textureId*4096u,512,512,20,0};
    draw.textures[0]=image;draw.samplers[0]=colorGradeSampler(false,2);
    put(draw.attributes.data()+92,0x00100008);draw.attributes[144]=draw.attributes[145]=2;
    return draw;
}
static void darknessArmBloomSeed(WorldRendererD3D11& renderer,const WorldDraw& draw,unsigned scale,
                                double gain,bool observeAlpha=false) {
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;
    clear.flags=1;clear.color={0,0,0,.3125f};renderer.clear(clear);
    require(renderer.draw(draw),"Darkness arm emissive shell rejected");
    const auto pixels=renderer.readSurface(draw.targets[0],false);
    const unsigned width=1280*scale;
    require(pixels.size()==size_t(width)*720*scale*8,"Darkness arm HDR scene extent changed");
    std::array<double,3> energy{},peak{};
    for(size_t n=0;n<pixels.size()/8;++n)for(unsigned c=0;c<4;++c) {
        uint16_t raw=0;std::memcpy(&raw,pixels.data()+n*8+c*2,2);
        const double value=materialHalf(raw);
        if(c<3) {energy[c]+=value;peak[c]=(std::max)(peak[c],value);}
        else if(!observeAlpha)require(value==.3125,"Darkness RGB shell changed framebuffer alpha");
    }
    for(unsigned c=0;c<3;++c) {
        const double radiance=draw.fragmentConstants[0][c]*gain;
        require(std::abs(peak[c]-radiance)<.002 &&
                std::abs(energy[c]/(scale*scale)-radiance*1024)<.02,
                "Darkness calibration lost its RGB palette, HDR peak or integrated radiance");
    }
    if(observeAlpha) {
        // Enable the alpha write solely to observe the shader's alpha. ONE/ONE
        // must add authored alpha without multiplying it by the RGB gain.
        uint16_t raw=0;std::memcpy(&raw,pixels.data()+(size_t(360*scale)*width+640*scale)*8+6,2);
        require(std::abs(materialHalf(raw)-(.3125+112.0/255*.5))<.0005,
                "Darkness RGB calibration multiplied the authored alpha");
    }
}
static BloomPixel darknessArmBloomScene(double u,double v,double gain) {
    const double px=u*1280-.5,py=v*720-.5;
    const int x=int(std::floor(px)),y=int(std::floor(py));
    const double fx=px-x,fy=py-y;double coverage=0;
    for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)
        if(x+int(dx)>=624 && x+int(dx)<656 && y+int(dy)>=344 && y+int(dy)<376)
            coverage+=(dx?fx:1-fx)*(dy?fy:1-fy);
    return {bloomRetailStored(4*gain)*coverage,bloomRetailStored(2*gain)*coverage,
            bloomRetailStored(gain)*coverage,.3125};
}
static double darknessArmBloomChain(WorldRendererD3D11& renderer,const WorldDraw& source,
                                    unsigned scale,double gain) {
    const auto scene=bloomRetailResolve(renderer,source,6901,false);
    auto shrink=colorGradeDraw(colorGradeQuad(160,90,1280,720),4802,1280,720);
    shrink.fragmentName="XRUtil_ShrinkTexture8";shrink.fragmentConstants[0]={1.0f/1280,1.0f/720,0,0};
    shrink.textureObjects[0]=scene;shrink.samplers[0]=colorGradeSampler(true,2);
    WorldClear clear;clear.targets=shrink.targets;clear.viewport=shrink.viewport;
    clear.flags=1;clear.color={16,0,16,1};renderer.clear(clear);
    bloomRetailResolve(renderer,shrink,6902,false);
    require(renderer.draw(shrink),"Darkness arm HDR shrink rejected");
    BloomImage reduced{160,90,{}};reduced.pixels.resize(160*90);
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x)
        for(int dy:{-3,-1,1,3})for(int dx:{-3,-1,1,3}) {
            const auto tap=darknessArmBloomScene((x+.5)/160+double(dx)/1280,
                                               (y+.5)/90+double(dy)/720,gain);
            for(unsigned c=0;c<4;++c)reduced.pixels[size_t(y)*160+x][c]+=tap[c]/16;
        }
    // At radiance 12, one FP16 storage step is .0078125. The preceding
    // UNORM16 fetch is just below 12 and may round down on either backend.
    reduced=bloomRetailCheck(renderer,shrink,scale,std::move(reduced),"arm-shrink",.008);
    auto atlas=bloomRetailResolve(renderer,shrink,6902,true);
    for(bool vertical:{false,true}) {
        auto gaussian=bloomRetailGaussian(vertical);gaussian.textureObjects[0]=atlas;
        renderer.clear(clear);require(renderer.draw(gaussian),"Darkness arm Gaussian rejected");
        reduced=bloomRetailCheck(renderer,gaussian,scale,bloomRetailGaussianOracle(reduced,gaussian),
                                vertical?"arm-negative-bias":"arm-horizontal",vertical?.004:.008);
        atlas=bloomRetailResolve(renderer,gaussian,6902,true);
    }
    auto lut=colorGradeDraw(colorGradeQuad(324,18,1280,720),6803,1280,720);
    lut.fragmentName="XREngine_CCFuser";lut.fragmentConstants[0]={1,1,1,1};
    lut.textures[1]=colorGradeCube(false);lut.samplers[1]=colorGradeSampler(false,2);
    bloomDraw(renderer,lut);const auto lookup=colorGradeResolve(renderer,lut,6903,324,18,26,4);
    auto final=colorGradeDraw(bloomRetailQuad(true),6804,1280,720);
    final.options.modes[1]=0;final.options.coordinates[1]=1;
    final.fragmentName="XREngine_Final5";final.fragmentFlags=14;
    final.textureObjects[0]=scene;final.textureObjects[1]=atlas;final.textureObjects[2]=lookup;
    for(unsigned slot=0;slot<3;++slot)final.samplers[slot]=colorGradeSampler(true,2);
    // Exposure and glow clamp are from the audited 720p red-power capture.
    final.fragmentConstants[0]={2.3930666447f,2.3930666447f,2.8244874477f,.3312084f};
    final.fragmentConstants[1]={.5f/1280,.5f/720,159.5f/1280,89.5f/720};bloomDraw(renderer,final);
    const auto pixels=renderer.readSurface(6804,false);const unsigned width=1280*scale;
    require(pixels.size()==size_t(width)*720*scale*8,"Darkness arm Final5 extent changed");
    auto actual=[&](unsigned x,unsigned y,unsigned c) {
        uint16_t raw=0;std::memcpy(&raw,pixels.data()+(size_t(y)*width+x)*8+c*2,2);
        return double(materialHalf(raw));
    };
    double maximum=0;unsigned comparisons=0,mismatches=0;
    for(unsigned y=248*scale;y<472*scale;y+=4)for(unsigned x=480*scale;x<800*scale;x+=4) {
        const auto glow=bloomSample(reduced,(x+.5)/width,(y+.5)/(720*scale));
        for(unsigned c=0;c<3;++c) {
            const double sceneValue=x/scale>=624 && x/scale<656 && y/scale>=344 && y/scale<376?
                (c==0?4:c==1?2:1)*gain:0;
            const double sceneSquared=sceneValue*sceneValue,glowSquared=glow[c]*glow[c];
            const double combined=sceneSquared+glowSquared-std::clamp(sceneSquared*glowSquared,0.0,1.0);
            const double transfer=std::sqrt((std::max)(1-std::exp(-(std::max)(combined*final.fragmentConstants[0][c],1e-7)),1e-8));
            const double position=std::clamp(transfer,0.0,1.0)*17;
            const unsigned lo=unsigned(std::floor(position)),hi=(std::min)(lo+1,17u);
            const double wanted=bloomRetailStored(lo/17.0)*(1-(position-lo))+bloomRetailStored(hi/17.0)*(position-lo);
            const double delta=std::abs(actual(x,y,c)-wanted);maximum=(std::max)(maximum,delta);++comparisons;
            if(!std::isfinite(delta) || delta>.004)++mismatches;
        }
    }
    const double halo=actual(720*scale,360*scale,0);
    require(mismatches==0,"Darkness arm halo differs from independent HDR/filter/Final5 oracle");
    require(actual(640*scale,456*scale,0)<.002,"Darkness arm calibration leaked beyond the authored blur extent");
    std::printf("DarknessArmBloom: scale=%u gain=%g halo=%g maxDelta=%.6f comparisons=%u\n",
                scale,gain,halo,maximum,comparisons);
    return halo;
}
static void darknessArmBloomContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    struct Restore {GraphicsSettings settings;~Restore(){setGraphicsSettings(settings);}} restore{graphicsSettings()};
    auto settings=restore.settings;settings.bloom=true;
    require(setGraphicsSettings(settings),"Cannot enable Darkness arm bloom fixture");
    for(unsigned scale:{1u,2u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);renderer.beginFrame();
        auto arm=darknessArmBloomDraw(9188),world=arm;world.textureIds[0]=9178;
        world.textureObjects[0].object=9178;world.textureObjects[0].storage=9178*4096u;
        // The source constant never changes: transition arm -> ordinary world
        // -> arm to exercise the renderer's adjusted constant upload cache.
        darknessArmBloomSeed(renderer,arm,scale,3);
        darknessArmBloomSeed(renderer,world,scale,1);
        const double worldHalo=darknessArmBloomChain(renderer,world,scale,1);
        darknessArmBloomSeed(renderer,arm,scale,3);
        const double armHalo=darknessArmBloomChain(renderer,arm,scale,3);
        require(worldHalo<.002 && armHalo>.20,
                "Darkness arm calibration failed to add a broad halo or changed an ordinary world source");
        settings.bloom=false;require(setGraphicsSettings(settings),"Cannot disable Darkness arm bloom fixture");
        renderer.beginFrame();darknessArmBloomSeed(renderer,arm,scale,1);
        settings.bloom=true;require(setGraphicsSettings(settings),"Cannot restore Darkness arm bloom fixture");
        renderer.beginFrame();darknessArmBloomSeed(renderer,arm,scale,3);
        if(scale==1) {
            for(uint16_t id:{uint16_t(9187),uint16_t(9189),uint16_t(9190),uint16_t(9192)}) {
                auto power=arm;power.textureIds[0]=id;
                power.textureObjects[0].object=id;power.textureObjects[0].storage=id*4096u;
                darknessArmBloomSeed(renderer,power,scale,3);
            }
            auto alpha=arm;put(alpha.attributes.data()+92,0x01100008);
            darknessArmBloomSeed(renderer,alpha,scale,3,true);
            // Reusing a signs image on a solid surface must not qualify just
            // because its ID and dimensions match the additive glow texture.
            auto solid=arm;put(solid.attributes.data()+92,0x00100000);
            darknessArmBloomSeed(renderer,solid,scale,1);
            auto depthWriting=arm;put(depthWriting.attributes.data()+92,0x0010000c);
            depthWriting.targets[4]=6805;darknessArmBloomSeed(renderer,depthWriting,scale,1);
            darknessArmBloomSeed(renderer,arm,scale,3);
        }
    }
    context->ClearState();
    std::puts("DarknessArmBloom passed: calibrated RGB through retail HDR bloom at scales1/2/3; ordinary world and bloom-off radiance preserved; all power signs, alpha and constant-cache transitions checked.");
}
