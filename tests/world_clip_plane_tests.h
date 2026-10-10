// The original reflection pass clips scene geometry against the water plane.
// Check coverage from analytic half-spaces, independently of the shader's DP4s.
static void worldClipPlanePass(WorldRendererD3D11& renderer,unsigned scale) {
    std::array<EngineVector,8> tex{};
    const auto geometry=materialQuad(tex);
    auto draw=materialDraw(geometry,3651);
    draw.fragmentName="MRenderXenon_Attrib_TexEnvMode00";
    // Guest clip Z=.25 becomes native Z=.75 with this reversed depth range.
    // A plane evaluated after that remap would keep the wrong half-space.
    draw.constants.vectors[2]={0,0,0,.25f};
    draw.depthRange={1,0,0,0};
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
    const unsigned side=32*scale;
    unsigned comparisons=0;
    auto verify=[&](auto visible,const char* label) {
        const auto pixels=renderer.readSurface(draw.targets[0],false);
        require(pixels.size()==size_t(side)*side*8,"Clip-plane output extent differs");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            const double px=(x+.5)*2/side-1,py=1-(y+.5)*2/side;
            const double expected=visible(px,py)?1:0;
            uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+(size_t(y)*side+x)*8,8);
            for(unsigned lane=0;lane<4;++lane) {
                if(std::abs(materialHalf(rgba[lane])-expected)>.001) {
                    std::fprintf(stderr,"ClipPlanes%u %s at%u,%u lane%u: actual=%g expected=%g\n",
                        scale,label,x,y,lane,materialHalf(rgba[lane]),expected);
                    require(false,"User clip-plane coverage differs from analytic half-space");
                }
                ++comparisons;
            }
        }
    };
    auto render=[&](auto visible,const char* label) {
        renderer.clear(clear);
        require(renderer.draw(draw),"Clip-plane fixture rejected");
        verify(visible,label);
    };
    const auto all=[](double,double){return true;};
    const auto none=[](double,double){return false;};
    const float nan=std::numeric_limits<float>::quiet_NaN();

    // Exercise every physical plane index, including sparse enable bit 5.
    // Disabled registers may contain stale nonfinite bits from earlier passes.
    for(unsigned slot=0;slot<6;++slot) {
        for(auto& plane:draw.clipPlanes.planes)plane={nan,nan,nan,nan};
        const float boundary=-.65f+.19f*slot;
        draw.clipPlanes.control=1u<<slot;
        draw.clipPlanes.planes[slot]={1,0,0,-boundary};
        render([=](double x,double){return x>=boundary;},"single sparse plane");
    }
    draw.clipPlanes.control=0x3f;
    draw.clipPlanes.planes={EngineVector{1,0,0,.73f},EngineVector{-1,0,0,.67f},
        EngineVector{0,1,0,.61f},EngineVector{0,-1,0,.79f},
        EngineVector{1,1,0,.43f},EngineVector{-1,-1,0,.53f}};
    render([](double x,double y) {
        return x>=-.73 && x<=.67 && y>=-.61 && y<=.79 && x+y>=-.43 && x+y<=.53;
    },"six intersecting planes");

    // Consecutive draws share geometry, shader, MVP, viewport and material.
    // Changing only the plane coefficients must update the bound clip bank.
    draw.clipPlanes={};draw.clipPlanes.control=1;
    draw.clipPlanes.planes[0]={1,0,0,-.5f};
    renderer.clear(clear);require(renderer.draw(draw),"Initial clip-bank draw rejected");
    draw.clipPlanes.planes[0]={-1,0,0,-.5f};
    require(renderer.draw(draw),"Changed clip-bank draw rejected");
    verify([](double x,double){return std::abs(x)>=.5;},"coefficient-only consecutive draws");

    // A different nonzero enable mask uses the same clipping shader variant.
    draw.clipPlanes.planes[0]={1,0,0,0};
    draw.clipPlanes.planes[5]={-1,0,0,0};
    renderer.clear(clear);require(renderer.draw(draw),"Initial clip-mask draw rejected");
    draw.clipPlanes.control=1u<<5;
    require(renderer.draw(draw),"Changed clip-mask draw rejected");
    verify(all,"mask-only consecutive draws");
    draw.clipPlanes.control=1;
    renderer.clear(clear);require(renderer.draw(draw),"Restored clip-mask draw rejected");
    draw.clipPlanes.control=0;
    require(renderer.draw(draw),"Disabled clip-mask draw rejected");
    verify(all,"zero mask after enabled draws");

    draw.clipPlanes.control=1;
    draw.clipPlanes.planes[0]={0,0,1,-.5f};
    render(none,"guest Z before reversed-depth remap");
    draw.clipPlanes.planes[0]={0,0,-1,.5f};
    render(all,"opposite guest-Z half-space");
    // CLIP_DISABLE takes precedence over enabled bits and stale coefficients.
    draw.clipPlanes.control=0x1003f;
    for(auto& plane:draw.clipPlanes.planes)plane={nan,nan,nan,nan};
    render(all,"clip disabled with stale NaN planes");
    draw.clipPlanes.control=0;
    render(all,"zero mask with stale NaN planes");

    // Cull-only drops a primitive only when all its vertices lie outside.
    // Both triangles cross X=0, so their negative portions remain visible.
    draw.clipPlanes={};draw.clipPlanes.control=0x20001;
    draw.clipPlanes.planes[0]={1,0,0,0};
    render(all,"cull-only crossing triangles");
    draw.clipPlanes.planes[0]={0,0,1,-.5f};
    render(none,"cull-only wholly outside triangles");
    draw.clipPlanes.control=1;
    draw.clipPlanes.planes[0]={1,0,0,0};
    render([](double x,double){return x>=0;},"clip variant after cull-only variant");

    std::printf("ClipPlanes%u: %u half-space coverage checks; six masks, immutable state updates, reversed depth and cull-only.\n",
        scale,comparisons);
}
