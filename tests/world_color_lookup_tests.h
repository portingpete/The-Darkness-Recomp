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
