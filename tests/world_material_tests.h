// Exercise shipped water/mark shaders with an independent color and plane oracle.
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
static void waterMaterialPass(WorldRendererD3D11& renderer,unsigned scale) {
    const std::array<uint8_t,4> reflected{192,64,32,255},refracted{32,96,160,255};
    const EngineVector tint{.5f,.75f,.25f,.8f};
    unsigned comparisons=0;
    for(const char* program:{"VBOp_FP20_Water","VBOp_FP20_CubeWater","VBOp_FP20_Water2","VBOp_FP20_CubeWater2"})
    for(bool grazing:{false,true}) {
        const bool cube=std::strstr(program,"Cube")!=nullptr;
        const bool second=program[std::strlen(program)-1]=='2';
        const EngineVector eye=grazing?EngineVector{0,1,0,0}:EngineVector{1,0,0,0};
        const EngineVector eyePosition=grazing?EngineVector{0,16,.5f,1}:EngineVector{16,0,.5f,1};
        std::array<EngineVector,8> tex{};tex[0]=tex[1]={.5f,.5f,0,1};
        if(second) {tex[2]={0,0,1,0};tex[3]={0,1,0,0};}
        else {tex[3]=eye;tex[4]={.5f,.5f,0,1};tex[5]={1,0,0,0};tex[6]={0,0,1,0};tex[7]={0,1,0,0};}
        auto draw=materialDraw(materialQuad(tex,second),3601);draw.fragmentName=program;draw.constants.vectors[10]=tint;
        if(second) {
            draw.options.normal=draw.options.tangents=true;
            draw.options.modes={0,0,8,11,23,24,4,4};
            draw.constants.vectors[8]={0,1,0,0};
            if(cube) {
                draw.options.modes[1]=20;draw.options.modes[2]=1;
                draw.constants.references[2][2]=40;draw.constants.vectors[40]=eyePosition;
                draw.constants.references[3][2]=44;
                draw.constants.vectors[44]={0,0,0,.5f};draw.constants.vectors[45]={0,0,0,.5f};
                draw.constants.vectors[47]={0,0,0,1};
            }
        }
        draw.textures[0]=materialSolid(reflected,cube);draw.textureObjects[0].faces=cube?6:1;
        draw.textures[1]=draw.textures[2]=materialSolid({128,128,128,255});
        draw.textures[4]=materialSolid(refracted);
        draw.fragmentConstants[2]={0,1,0,0};draw.fragmentConstants[3]={0,0,1,0};
        if(second) {
            draw.fragmentConstants[4]=eyePosition;
            draw.fragmentConstants[5]={0,0,0,.5f};draw.fragmentConstants[6]={0,0,0,.5f};
            draw.fragmentConstants[7]={0,0,0,0};draw.fragmentConstants[8]={0,0,0,1};
        }
        WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;
        renderer.clear(clear);require(renderer.draw(draw),"Original water material was omitted");
        const auto pixels=renderer.readSurface(draw.targets[0],false);
        const unsigned side=32*scale;
        require(pixels.size()==size_t(side)*side*8,"Water material output extent differs");
        // The planar Water2 shader doubles the original normal amplitude.
        const double normal=(2.0*128/255-1)*6*.1666*(second&&!cube?1:.5);
        for(unsigned y=side/4;y<side*3/4;++y)for(unsigned x=side/4;x<side*3/4;++x) {
            double facing=grazing?normal:std::sqrt(1-2*normal*normal);
            if(second) {
                const double dx=eyePosition[0]-((x+.5)*2/side-1),dy=eyePosition[1]-(1-(y+.5)*2/side);
                facing=(std::sqrt(1-2*normal*normal)*dx+normal*dy)/std::sqrt(dx*dx+dy*dy);
            }
            const double weight=(std::max)(.1,1-std::clamp(facing,0.0,1.0))*tint[3];
            uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+(size_t(y)*side+x)*8,8);
            for(unsigned l=0;l<4;++l) {
                const double expected=l==3?1:weight*reflected[l]/255+(1-weight)*refracted[l]/255*tint[l];
                if(std::abs(materialHalf(rgba[l])-expected)>=.003) {
                    std::fprintf(stderr,"Water mismatch %s grazing=%u scale=%u at %u,%u lane%u: actual=%g expected=%g weight=%g\n",
                        program,unsigned(grazing),scale,x,y,l,materialHalf(rgba[l]),expected,weight);
                    require(false,"Water lost Fresnel, reflection, refraction or tint");
                }
                ++comparisons;
            }
        }
    }
    std::printf("WaterMaterials%u: %u reflection/refraction/Fresnel color checks passed.\n",scale,comparisons);
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
