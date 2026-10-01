#pragma once

// The same original YUV fragment runs on stored/world geometry as on UI
// rectangles. Exercise its actual D3D11 path with decoder-order V,U pixels,
// chroma's L,L,L,A view, tint, output alpha and immutable frame generations.
static void worldVideoPass(WorldRendererD3D11& renderer,unsigned scale) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=3;geometry->stride=32;
    geometry->formats[0]=geometry->formats[1]=4;
    geometry->vertices.resize(3*32);geometry->indices={0,1,2};
    const EngineVector positions[]{{-.75f,-.75f,.5f,1},{0,.75f,.5f,1},{.75f,-.75f,.5f,1}};
    for(unsigned vertex=0;vertex<3;++vertex)for(unsigned lane=0;lane<4;++lane) {
        put(geometry->vertices.data()+vertex*32+lane*4,std::bit_cast<uint32_t>(positions[vertex][lane]));
        put(geometry->vertices.data()+vertex*32+16+lane*4,std::bit_cast<uint32_t>(lane<2?.5f:lane==3?1.0f:0.0f));
    }
    WorldDraw draw;draw.geometry={geometry,geometry,0,3};
    draw.targets={3502,0,0,0,0};draw.viewport={0,0,64,64};
    draw.material=WorldMaterial::post;draw.fragmentName="CMWnd_ModTexture_PaintVideo_YUV2RGB";
    draw.options.modes.fill(4);draw.options.modes[0]=0;
    for(unsigned lane=0;lane<4;++lane)draw.constants.vectors[lane][lane]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    for(unsigned slot=0;slot<2;++slot) {
        auto& sampler=draw.samplers[slot];sampler.valid=sampler.lodValid=true;
        sampler.baseOnly=true;sampler.address.fill(2);
    }
    std::vector<uint8_t> source(4096);
    constexpr uint32_t image=64,pixels=512;
    auto plane=[&](unsigned w,unsigned h,unsigned bpp,unsigned format,uint8_t a,uint8_t b=0) {
        std::fill(source.begin(),source.end(),uint8_t(0));
        put(source.data()+image,0x82097610);put(source.data()+image+8,pixels);
        put(source.data()+image+12,w*h*bpp);put(source.data()+image+16,w);put(source.data()+image+20,h);
        put(source.data()+image+24,w*bpp);put(source.data()+image+28,bpp);
        put(source.data()+image+32,format);put(source.data()+image+40,0x810);
        for(unsigned i=0;i<w*h;++i) {source[pixels+i*bpp]=a;if(bpp==2)source[pixels+i*bpp+1]=b;}
        auto result=std::make_shared<ColorImage>();
        require(!decodeUploadImage(source.data(),image,pixels,*result),"Video world plane upload rejected");
        result->authoredMips=true;
        return result;
    };
    const auto redY=plane(2,2,1,0x2000,81),redUV=plane(1,1,2,0x20000,240,90);
    const auto blueY=plane(2,2,1,0x2000,41),blueUV=plane(1,1,2,0x20000,110,240);
    const auto greenY=plane(2,2,1,0x2000,145),greenUV=plane(1,1,2,0x20000,34,54);
    const auto whiteY=plane(2,2,1,0x2000,235),whiteUV=plane(1,1,2,0x20000,128,128);
    std::fill(source.begin(),source.end(),uint8_t(0));
    require(redUV->pixels==std::vector<uint8_t>({90,90,90,240}),"Chroma frame view/order or ownership changed");
    auto halfFloat=[](uint16_t h) {
        const unsigned exponent=(h>>10)&31,mantissa=h&1023;
        return std::ldexp(double(exponent?1024+mantissa:mantissa),int(exponent?exponent:1)-25)*(h&0x8000?-1:1);
    };
    auto render=[&](std::shared_ptr<const ColorImage> y,std::shared_ptr<const ColorImage> uv,
                    const std::array<uint8_t,3>& expected) {
        draw.textures[0]=std::move(y);draw.textures[1]=std::move(uv);
        WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
        renderer.clear(clear);require(renderer.draw(draw),"Original world/television YUV draw rejected");
        const auto output=renderer.readSurface(3502,false);const unsigned side=64*scale;
        require(output.size()==size_t(side)*side*8,"World video output dimensions differ");
        uint16_t rgba[4]{};std::memcpy(rgba,output.data()+(size_t(side/2)*side+side/2)*8,8);
        for(unsigned channel=0;channel<3;++channel)
            require(std::abs(halfFloat(rgba[channel])-expected[channel]/255.0)<.006,
                    "Original world video swapped U/V or lost YUV conversion");
        require(rgba[3]==0,"Original world video's zero alpha changed");
        const uint16_t black[4]{};
        require(!std::memcmp(output.data(),black,sizeof(black)),"World video escaped its original geometry");
    };
    render(redY,redUV,{254,0,0});render(blueY,blueUV,{0,0,255});
    render(greenY,greenUV,{0,255,1});render(whiteY,whiteUV,{255,254,255});
    render(redY,redUV,{254,0,0}); // Revisit an earlier queued frame generation.
    draw.constants.vectors[10]={.5f,.75f,.25f,1};
    render(redY,redUV,{127,0,0});
    std::printf("WorldVideo%u: original YUV program, linear chroma upload, colors/tint, alpha and retained frames passed.\n",scale);
}
