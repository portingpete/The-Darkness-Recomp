// The shipped Xenon shadow projector reads both the scene depth and a resolved
// depth atlas. Its PCF footprint is four logical atlas texels at every scale.
static void shadowProjectionPass(WorldRendererD3D11& renderer,const WorldDraw& seed,unsigned scale) {
    WorldClear shadow;shadow.targets[4]=3101;shadow.viewport={0,0,64,64};shadow.flags=48;
    WorldResolve shadowCopy;shadowCopy.targets=shadow.targets;shadowCopy.flags=4;
    shadowCopy.rectangle={0,0,64,64};shadowCopy.destination={3104,0x3104000,64,64,23,0,1};
    WorldClear scene=shadow;scene.targets[4]=3102;scene.depth=.5f;renderer.clear(scene);
    WorldResolve sceneCopy=shadowCopy;sceneCopy.targets=scene.targets;
    sceneCopy.destination={3105,0x3105000,64,64,23,0,1};
    require(renderer.resolve(sceneCopy),"Scene depth resolve for shadow projector rejected");

    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;
    binding.descriptor.modes.fill(4);binding.descriptor.modes[0]=7;
    binding.descriptor.parameters[0][0]=20;
    const auto bytes=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|bytes[i*4+n];
    const EngineVector center{.5f,.5f,0,1};
    for(unsigned lane=0;lane<4;++lane)
        put(binding.constantBytes.data()+20*16+lane*4,std::bit_cast<uint32_t>(center[lane]));
    auto draw=seed;require(prepareWorldVertexProgram(binding,draw.options,draw.constants),"Shadow projector vertex binding failed");
    draw.targets={3103,0,0,0,0};draw.material=WorldMaterial::post;
    draw.fragmentName="XREngine_ShadowProj";draw.fragmentFlags=8;draw.fragmentConstants={};
    draw.fragmentConstants[1]={0,0,1,-1}; // Finite scene-depth reconstruction.
    draw.fragmentConstants[5]={0,0,0,.5f}; // Projected depth becomes 1 - .5.
    draw.fragmentConstants[6]={0,0,0,1};
    draw.fragmentConstants[9]={1.0f/64,1.0f/64,0,0};
    draw.textureObjects[0]=shadowCopy.destination;draw.textureObjects[1]=sceneCopy.destination;
    draw.textures[0].reset();draw.textures[1].reset();
    for(unsigned slot=0;slot<2;++slot) {
        auto& sampler=draw.samplers[slot];sampler.valid=sampler.lodValid=true;
        sampler.minLevel=sampler.maxLevel=0;sampler.address.fill(2);
    }
    draw.attributes.fill(0);put(draw.attributes.data()+92,0x01100000);draw.attributes[97]=8;
    WorldClear target;target.targets=draw.targets;target.viewport=draw.viewport;target.flags=1;
    auto halfFloat=[](uint16_t h) {
        const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);
    };
    for(unsigned pattern=0;pattern<5;++pattern) {
        shadow.depth=pattern==1?.75f:.25f;renderer.clear(shadow);
        if(pattern>=2) {
            auto right=shadow;right.rectangle=std::array<int32_t,4>{32,0,64,64};right.depth=.75f;
            renderer.clear(right);
        }
        require(renderer.resolve(shadowCopy),"Shadow atlas depth resolve rejected");
        // One logical pixel off the seam must still see the opposite side at
        // 2x/3x. A filter accidentally measured in physical pixels will not.
        draw.fragmentConstants[3][3]=pattern==3?-1.0f/64:pattern==4?1.0f/64:0;
        renderer.clear(target);require(renderer.draw(draw),"Original shadow projector draw rejected");
        const auto pixels=renderer.readSurface(3103,false);
        const unsigned side=64*scale;require(pixels.size()==size_t(side)*side*8,"Shadow output scale differs");
        uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+(size_t(side/2)*side+side/2)*8,8);
        const double expected=pattern==0?1.0:pattern==1?0.0:pattern==2?0.5:pattern==3?0.75:0.25;
        require(std::abs(halfFloat(rgba[0])-expected)<.01 &&
                std::abs(halfFloat(rgba[3])-(1-expected))<.01,
                "Shadow PCF/lighting differs between logical and physical resolutions");
    }
    std::printf("ShadowProjection%u: lit, occluded and three 4x4 penumbra positions passed.\n",scale);
}
