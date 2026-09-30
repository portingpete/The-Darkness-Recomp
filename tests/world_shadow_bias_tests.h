// Shadow casters carry the original polygon offset in attribute bytes100/104.
// Read back actual D24 pixels to cover sign, slope, scale and cached-state reset.
static void shadowBiasPass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    auto draw=seed;draw.targets={0,0,0,0,3401};draw.depthRange={1,0,0,0};
    draw.attributes.fill(0);draw.attributes[96]=8;
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;
    clear.flags=48;clear.stencil=0xA5;
    const auto centerDepth=[&]() {
        const auto pixels=renderer.readSurface(3401,true);
        const unsigned side=64*scale;
        require(pixels.size()==size_t(side)*side*4,"Shadow bias output extent differs");
        uint32_t packed=0;std::memcpy(&packed,pixels.data()+(size_t(side/2)*side+side/2)*4,4);
        require((packed>>24)==0xA5,"Shadow bias changed stencil");
        return double(packed&0xFFFFFF)/0xFFFFFF;
    };
    for(const auto& gradient:std::array<std::array<float,2>,4>{{{0,0},{.1f,0},{0,-.2f},{.2f,-.15f}}}) {
        auto geometry=std::make_shared<StoredGeometry>();
        geometry->vertexCount=4;geometry->stride=16;geometry->formats[0]=4;
        geometry->vertices.resize(64);geometry->indices={0,1,2,0,2,3};
        const float xy[4][2]{{-1,-1},{-1,1},{1,1},{1,-1}};
        for(unsigned i=0;i<4;++i) {
            const EngineVector position{xy[i][0],xy[i][1],.5f+xy[i][0]*gradient[0]+xy[i][1]*gradient[1],1};
            for(unsigned lane=0;lane<4;++lane)
                put(geometry->vertices.data()+i*16+lane*4,std::bit_cast<uint32_t>(position[lane]));
        }
        draw.geometry.vertices=draw.geometry.indices=geometry;draw.geometry.indexCount=6;
        put(draw.attributes.data()+92,6);renderer.clear(clear);
        require(renderer.draw(draw),"Unbiased shadow caster rejected");
        const auto baseline=centerDepth();
        for(const auto& bias:std::array<std::array<float,2>,4>{{{0,5},{5,0},{5,5},{-2,-3}}}) {
            put(draw.attributes.data()+92,0x40006);
            put(draw.attributes.data()+100,std::bit_cast<uint32_t>(bias[0]));
            put(draw.attributes.data()+104,std::bit_cast<uint32_t>(bias[1]));
            renderer.clear(clear);require(renderer.draw(draw),"Biased shadow caster rejected");
            // Clip x/y span two units over64 logical pixels. The original
            // reversed-depth units are -2^-19, independently verified against
            // the executable by WorldRenderStateContract.
            const double slope=2.0*(std::max)(std::abs(gradient[0]),std::abs(gradient[1]))/64;
            const double expected=baseline-bias[0]*slope-bias[1]/524288.0;
            require(std::abs(centerDepth()-expected)<4.0/0xFFFFFF,
                    "Shadow caster lost original slope/constant bias or resolution scaling");
            // The following draw retains attribute bytes but disables bias.
            put(draw.attributes.data()+92,6);renderer.clear(clear);
            require(renderer.draw(draw),"Unbiased draw after shadow caster rejected");
            require(centerDepth()==baseline,"Shadow bias leaked into the next draw");
        }
    }
    std::printf("ShadowBias%u: flat/sloped D24 offsets, scale and state reset passed.\n",scale);
}
