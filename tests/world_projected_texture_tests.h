// TexEnvProj1 is the original generic projected-texture pass. A non-unit Q
// must select xy/Q before texture sampling, with color and fetch scaling kept.
static void projectedTexturePass(WorldRendererD3D11& renderer,unsigned scale) {
    auto texture=std::make_shared<ColorImage>();
    texture->width=texture->height=2;
    texture->pixels={255,0,0,255, 0,255,0,128,
                     0,0,255,64, 255,255,255,0};
    const EngineVector tint{.25f,.5f,.75f,.5f};
    struct Projection {float s,t,q;unsigned texel;};
    const Projection cases[]{{.75f,.25f,1,1}, {.75f,.25f,2,0},
                             {1.5f,.5f,2,1}, {.5f,1.5f,2,2},
                             {1.5f,1.5f,2,3}, {-1.5f,-.5f,-2,1}};
    auto halfFloat=[](uint16_t h) {
        const unsigned exponent=(h>>10)&31,mantissa=h&1023;
        return std::ldexp(double(exponent?1024+mantissa:mantissa),int(exponent?exponent:1)-25)*(h&0x8000?-1:1);
    };
    for(const auto& projected:cases) {
        auto geometry=std::make_shared<StoredGeometry>();
        geometry->vertexCount=3;geometry->stride=32;
        geometry->formats[0]=geometry->formats[1]=4;
        geometry->vertices.resize(3*32);geometry->indices={0,1,2};
        const EngineVector positions[]{{-.75f,-.75f,.5f,1},{0,.75f,.5f,1},{.75f,-.75f,.5f,1}};
        const EngineVector coordinate{projected.s,projected.t,0,projected.q};
        for(unsigned vertex=0;vertex<3;++vertex)for(unsigned lane=0;lane<4;++lane) {
            put(geometry->vertices.data()+vertex*32+lane*4,std::bit_cast<uint32_t>(positions[vertex][lane]));
            put(geometry->vertices.data()+vertex*32+16+lane*4,std::bit_cast<uint32_t>(coordinate[lane]));
        }
        WorldDraw draw;draw.geometry={geometry,geometry,0,3};
        draw.targets={3501,0,0,0,0};draw.viewport={0,0,64,64};
        draw.material=WorldMaterial::post;draw.fragmentName="TexEnvProj1";
        draw.options.modes.fill(4);draw.options.modes[0]=0;
        for(unsigned lane=0;lane<4;++lane)draw.constants.vectors[lane][lane]=1;
        draw.constants.references[0][2]=10;draw.constants.vectors[10]=tint;
        draw.textures[0]=texture;draw.textureObjects[0].exponent=1;
        auto& sampler=draw.samplers[0];sampler.valid=sampler.lodValid=true;
        sampler.baseOnly=true;sampler.address.fill(2);
        put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
        WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
        renderer.clear(clear);
        require(renderer.draw(draw),"Original TexEnvProj1 draw rejected");
        const auto pixels=renderer.readSurface(3501,false);
        const unsigned side=64*scale;
        require(pixels.size()==size_t(side)*side*8,"Projected texture output scale differs");
        uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+(size_t(side/2)*side+side/2)*8,8);
        for(unsigned lane=0;lane<4;++lane) {
            const double expected=double(texture->pixels[projected.texel*4+lane])/255*tint[lane]*2;
            require(std::abs(halfFloat(rgba[lane])-expected)<.002,
                    "Projected texture lost Q division, vertex color or fetch scale");
        }
        const uint16_t black[4]{};
        require(!std::memcmp(pixels.data(),black,sizeof(black)),"Projected texture escaped its geometry");
    }
    std::printf("ProjectedTexture%u: six Q/texel cases, vertex color and fetch scaling passed.\n",scale);
}
