// Exercise shipped decal shaders with an independent color and plane oracle.
static double materialHalf(uint16_t h) {
    const unsigned e=(h>>10)&31,m=h&1023;
    return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);
}
static std::shared_ptr<ColorImage> materialSolid(std::array<uint8_t,4> rgba,bool cube=false) {
    auto image=std::make_shared<ColorImage>();image->width=image->height=1;image->faces=cube?6:1;
    for(unsigned f=0;f<image->faces;++f)image->pixels.insert(image->pixels.end(),rgba.begin(),rgba.end());
    return image;
}
static std::shared_ptr<StoredGeometry> materialQuad(const std::array<EngineVector,8>& tex,bool normal=false) {
    const unsigned streams=normal?10:9;
    auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=4;geometry->stride=streams*16;
    for(unsigned s=0;s<streams;++s)geometry->formats[s]=4;
    geometry->vertices.resize(4*geometry->stride);geometry->indices={0,1,2,0,2,3};
    const EngineVector positions[]{{-1,-1,.5f,1},{-1,1,.5f,1},{1,1,.5f,1},{1,-1,.5f,1}};
    const EngineVector basisNormal{1,0,0,0};
    for(unsigned v=0;v<4;++v)for(unsigned s=0;s<streams;++s)for(unsigned l=0;l<4;++l)
        put(geometry->vertices.data()+v*geometry->stride+s*16+l*4,
            std::bit_cast<uint32_t>((s==9?basisNormal:s?tex[s-1]:positions[v])[l]));
    return geometry;
}
static WorldDraw materialDraw(std::shared_ptr<StoredGeometry> geometry,uint32_t target) {
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};draw.targets={target,0,0,0,0};
    draw.viewport={0,0,32,32};draw.material=WorldMaterial::post;
    draw.options.modes.fill(0);
    for(unsigned s=0;s<8;++s)draw.options.coordinates[s]=uint8_t(s);
    for(unsigned l=0;l<4;++l)draw.constants.vectors[l][l]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    for(auto& sampler:draw.samplers) {sampler.valid=sampler.lodValid=sampler.baseOnly=true;sampler.address.fill(2);}
    return draw;
}
static void projectedMarkPass(WorldRendererD3D11& renderer,unsigned scale) {
    std::array<EngineVector,8> tex{};
    auto geometry=materialQuad(tex);
    // Use the original stage-1 world-position mode captured on blood decals.
    // A quarter-width mark must clip against its plane and both UV bounds.
    auto draw=materialDraw(geometry,3602);draw.fragmentName="XRShader_DecalTMProj";
    draw.options.modes[1]=10;
    draw.textures[0]=materialSolid({192,32,16,128});
    draw.fragmentConstants[0]={0,0,0,1};draw.fragmentConstants[1]={0,0,1,.2f};
    draw.fragmentConstants[2]={1,0,0,1};draw.fragmentConstants[3]={0,1,0,0};
    draw.fragmentConstants[4]={.5f,.75f,.25f,.8f};
    const unsigned side=32*scale;
    for(bool offPlane:{false,true}) {
        draw.fragmentConstants[0][2]=offPlane?1.5f:.5f;
        WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
        renderer.clear(clear);require(renderer.draw(draw),"Original projected blood/mark material was omitted");
        const auto pixels=renderer.readSurface(draw.targets[0],false);
        require(pixels.size()==size_t(side)*side*8,"Projected mark output extent differs");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            const double wx=(x+.5)*2/side-1,wy=1-(y+.5)*2/side;
            const bool inside=!offPlane && std::abs(wx)<.5 && std::abs(wy)<.5;
            uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+(size_t(y)*side+x)*8,8);
            const uint8_t sample[]{192,32,16,128};
            for(unsigned l=0;l<4;++l)require(std::abs(materialHalf(rgba[l])-(inside?sample[l]/255.0*draw.fragmentConstants[4][l]:0))<.002,
                "Projected blood/mark escaped UV/plane clipping or lost tint/alpha");
        }
    }
    std::printf("ProjectedMarks%u: plane thickness, UV footprint, tint and alpha passed.\n",scale);
}
