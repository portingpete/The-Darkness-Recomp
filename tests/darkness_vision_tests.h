#pragma once

// Exercise both original DV5 stages on a scene and a lower-right effect atlas.
// Uniform depth/noise cancels edge glow, giving independent analytic RGB
// results. A second depth selects the original Xenon far-plane discard.
static void darknessVisionContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=4;geometry->stride=44;geometry->formats[0]=3;
    for(unsigned s=1;s<=4;++s)geometry->formats[s]=2;
    geometry->vertices.resize(4*44);geometry->indices={0,1,2,0,2,3};
    const float uv[4][2]{{0,1},{0,0},{1,0},{1,1}};
    for(unsigned v=0;v<4;++v) {
        auto* p=geometry->vertices.data()+v*44;
        const float position[]{uv[v][0]*2-1,1-uv[v][1]*2,.5f};
        for(unsigned c=0;c<3;++c)put(p+c*4,std::bit_cast<uint32_t>(position[c]));
        for(unsigned s=0;s<4;++s)for(unsigned c=0;c<2;++c) {
            const float value=s==0?uv[v][c]:s==2?uv[v][c]*2-1:.5f+.5f*uv[v][c];
            put(p+12+s*8+c*4,std::bit_cast<uint32_t>(value));
        }
    }
    using Rgba=std::array<uint8_t,4>;
    const Rgba colors[]{{32,64,96,192},{96,128,160,192},{160,96,32,192},{128,64,32,192}};
    const Rgba effects[]{{16,32,48,64},{48,64,16,96},{64,16,32,128},{32,48,64,160}};
    auto scene=std::make_shared<ColorImage>();scene->width=scene->height=2;
    for(const auto& color:colors)scene->pixels.insert(scene->pixels.end(),color.begin(),color.end());
    auto atlas=std::make_shared<ColorImage>();atlas->width=atlas->height=4;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        const auto color=x>=2 && y>=2?effects[(y-2)*2+x-2]:Rgba{255,0,255,255};
        atlas->pixels.insert(atlas->pixels.end(),color.begin(),color.end());
    }
    auto zero=std::make_shared<ColorImage>();zero->width=zero->height=1;zero->pixels={0,0,0,255};
    auto one=std::make_shared<ColorImage>(*zero);one->pixels={255,255,255,255};
    auto half=[](uint16_t h) {unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    unsigned checks=0;
    for(unsigned scale:{1u,2u,3u}) {
        WorldRendererD3D11 renderer(device,context,scale);
        for(unsigned stage:{0u,1u})for(unsigned flags:{0u,1u,2u})for(bool beyondFarPlane:{false,true}) {
            WorldDraw draw;draw.geometry={geometry,geometry,0,6};draw.material=WorldMaterial::post;
            draw.fragmentName=stage==0?"WClientMod_DV5_0":"WClientMod_DV5_1";draw.fragmentFlags=flags;
            draw.textureMask=worldFragmentTextureMask(draw.fragmentName,flags);
            require(draw.textureMask==(stage==0?(flags?0x37:0x17):(flags?3:1)),"DV5 capture mask is incomplete");
            draw.options.modes.fill(4);draw.options.modes[0]=0;
            draw.options.coordinates={0,1,2,3,4,5,6,7};
            if(stage==0)draw.options.modes[2]=0;
            if(flags)draw.options.modes[stage==0?3:1]=0;
            for(unsigned c=0;c<4;++c)draw.constants.vectors[c][c]=1;
            draw.textures[0]=scene;
            if(stage==0) {draw.textures[1]=beyondFarPlane?one:zero;draw.textures[2]=draw.textures[4]=zero;}
            if(flags)draw.textures[stage==0?5:1]=atlas;
            for(auto& sampler:draw.samplers) {sampler.valid=true;sampler.address.fill(2);}
            draw.fragmentConstants[0]={1.f/64,1.f/24,0,0};
            draw.fragmentConstants[1]={.1f,.2f,.3f,1};
            draw.fragmentConstants[3]={2,0,2400,-2};
            draw.fragmentConstants[4]={0,0,1,1};
            for(unsigned c=0;c<3;++c)draw.fragmentConstants[5+c][c]=1;
            draw.viewport={0,0,64,24};draw.targets={3901,0,0,0,0};
            put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
            WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=1;clear.color={.25f,.125f,.5f,.75f};
            renderer.clear(clear);require(renderer.draw(draw),"Original Darkness Vision shader rejected");
            const auto pixels=renderer.readSurface(3901,false);
            require(pixels.size()==64*24*scale*scale*8,"DV5 output size changed");
            for(unsigned y=0;y<24*scale;++y)for(unsigned x=0;x<64*scale;++x) {
                const unsigned quadrant=(y>=12*scale?2:0)+(x>=32*scale?1:0);
                uint16_t pixel[4];std::memcpy(pixel,pixels.data()+(size_t(y)*64*scale+x)*8,8);
                for(unsigned c=0;c<4;++c) {
                    double expected=colors[quadrant][c]/255.0;
                    if(c==3 && stage==0)expected=1;
                    if(c<3 && flags==1)expected+=effects[quadrant][c]/255.0;
                    if(c<3 && flags==2)expected*=effects[quadrant][c]/255.0;
                    if(stage==0 && beyondFarPlane)expected=clear.color[c];
                    require(std::isfinite(half(pixel[c])) && std::abs(half(pixel[c])-expected)<.002,
                            "Darkness Vision differs from scene/atlas blend or Xenon depth discard");
                    ++checks;
                }
            }
        }
        context->ClearState();
    }
    std::printf("DarknessVision: %u RGBA checks; both stages, all authored variants, atlas expansion, far discard, scale 1/2/3.\n",checks);
}
