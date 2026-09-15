// Exercise the original textured cutout path on the native single-sample
// surfaces. Transparent fragments must leave color, depth and stencil intact.
static void alphaCoveragePass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    auto draw=seed;
    draw.targets={2911,0,0,0,2912};
    draw.material=WorldMaterial::fixed;
    draw.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    draw.fragmentFlags=0;
    draw.fragmentConstants={}; // Original texture * vertex color mode.
    auto binding=fixture(0,false);
    binding.descriptor.flags=0x03000000;
    binding.descriptor.modes.fill(4);
    binding.descriptor.modes[0]=7;
    binding.descriptor.parameters[0][0]=20;
    const auto bytes=encodeEngineVertexDescriptor(binding.descriptor);
    binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|bytes[i*4+n];
    for(unsigned lane=0;lane<4;++lane) {
        put(binding.constantBytes.data()+7*16+lane*4,0);
        put(binding.constantBytes.data()+10*16+lane*4,std::bit_cast<uint32_t>(1.0f));
        put(binding.constantBytes.data()+20*16+lane*4,std::bit_cast<uint32_t>(.5f));
    }
    require(prepareWorldVertexProgram(binding,draw.options,draw.constants),"Cutout vertex binding failed");
    draw.attributes.fill(0);
    draw.attributes[96]=8;
    draw.attributes[120]=0x80;draw.attributes[121]=2;
    draw.attributes[124]=93;draw.attributes[125]=draw.attributes[126]=255;
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;
    clear.flags=49;clear.color={.125f,0,0,1};clear.depth=0;clear.stencil=17;
    const size_t pixels=size_t(64*scale)*(64*scale);
    unsigned cases=0;
    std::array<unsigned,2> previousCoverage{};
    for(bool colorWrites:{true,false})for(unsigned alpha:{0u,64u,127u,128u,192u,255u}) {
        auto texture=std::make_shared<ColorImage>();
        texture->width=texture->height=1;texture->pixels={0,255,0,uint8_t(alpha)};
        draw.textures[0]=texture;
        // Exercise both transitions through the state cache, including an
        // irrelevant NEVER comparison/reference while coverage is enabled.
        for(bool coverage:{false,true,false}) {
            put(draw.attributes.data()+92,(colorWrites?0x01100000u:0u)|0x4006u|unsigned(coverage));
            draw.attributes[97]=coverage?1:8;
            draw.attributes[98]=draw.attributes[99]=255;
            renderer.clear(clear);
            const auto background=renderer.readSurface(2911,false);
            const auto emptyDepth=renderer.readSurface(2912,true);
            require(renderer.draw(draw),"Textured cutout draw rejected");
            const auto color=renderer.readSurface(2911,false);
            const auto depth=renderer.readSurface(2912,true);
            require(color.size()==pixels*8 && background.size()==color.size() &&
                    depth.size()==pixels*4 && emptyDepth.size()==depth.size(),"Cutout surface extent differs");
            unsigned covered=0;
            for(size_t pixel=0;pixel<pixels;++pixel) {
                uint16_t rgba[4]{};uint32_t z=0;
                std::memcpy(rgba,color.data()+pixel*8,8);
                std::memcpy(&z,depth.data()+pixel*4,4);
                const bool drawn=(z>>24)==93;
                if(drawn) {
                    ++covered;
                    require(std::abs(double(z&0xFFFFFF)/0xFFFFFF-.5)<1e-6,
                            "Cutout color/depth/stencil writes disagree");
                    require(colorWrites?(rgba[0]==0 && rgba[1]==0x3c00 && rgba[2]==0):
                            !std::memcmp(color.data()+pixel*8,background.data()+pixel*8,8),
                            "Cutout color write mask was not preserved");
                } else {
                    require(!std::memcmp(color.data()+pixel*8,background.data()+pixel*8,8) &&
                            !std::memcmp(depth.data()+pixel*4,emptyDepth.data()+pixel*4,4),
                            "Transparent cutout changed background or depth/stencil");
                }
            }
            const unsigned footprint=1152*scale*scale;
            // D3D leaves intermediate alpha coverage to the implementation:
            // WARP dithers where some hardware uses a single-sample cutoff.
            // Require the specified endpoints and monotone coverage, not one
            // driver's particular pattern for partially transparent edges.
            require(covered<=footprint,"Cutout coverage escaped the triangle");
            if(!coverage || alpha==255)require(covered==footprint,"Opaque cutout lost coverage");
            if(coverage) {
                if(!alpha)require(!covered,"Transparent foliage wrote solid depth");
                require(covered>=previousCoverage[colorWrites],"Increasing alpha reduced foliage coverage");
                previousCoverage[colorWrites]=covered;
            }
            if(!colorWrites) {
                // The subsequent opaque lighting pass uses EQUAL depth and
                // must shade only the pixels admitted by the cutout prepass.
                auto lighting=draw;
                put(lighting.attributes.data()+92,0x01100002u);
                lighting.attributes[96]=3;lighting.attributes[97]=8;
                require(renderer.draw(lighting),"Cutout lighting pass rejected");
                const auto lit=renderer.readSurface(2911,false);
                const auto litDepth=renderer.readSurface(2912,true);
                require(litDepth==depth,"Lighting pass changed the cutout depth mask");
                for(size_t pixel=0;pixel<pixels;++pixel) {
                    uint32_t z=0;uint16_t green=0;
                    std::memcpy(&z,depth.data()+pixel*4,4);
                    std::memcpy(&green,lit.data()+pixel*8+2,2);
                    require((z>>24)==93?green==0x3c00:
                            !std::memcmp(lit.data()+pixel*8,background.data()+pixel*8,8),
                            "Lighting filled transparent areas of a foliage card");
                }
            }
            ++cases;
        }
    }
    std::printf("AlphaCoverage%u: %u textured cases; coverage bounds, background/depth/stencil, lighting and state transitions passed.\n",scale,cases);
}
