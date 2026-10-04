// The original model-opacity path saves the scene before drawing the model,
// then restores that RGB with alpha 1-opacity over a cropped screen rectangle.
// Sampled alpha is replaced by env0.x, rather than multiplying the whole RGBA.
static void transparentMeshPass(WorldRendererD3D11& renderer,unsigned scale) {
    constexpr uint32_t colorTarget=4571,depthTarget=4572;
    auto rectangle=[](bool cropped) {
        auto geometry=std::make_shared<StoredGeometry>();
        geometry->vertexCount=4;geometry->stride=32;
        geometry->formats[0]=geometry->formats[1]=4;
        geometry->vertices.resize(4*32);geometry->indices={0,1,2,0,2,3};
        const float bound=cropped?.5f:1.0f,low=cropped?.25f:0.0f,high=1-low;
        const EngineVector positions[]{{-bound,-bound,.5f,1},{-bound,bound,.5f,1},
                                       {bound,bound,.5f,1},{bound,-bound,.5f,1}};
        const EngineVector coordinates[]{{low,high,0,1},{low,low,0,1},
                                         {high,low,0,1},{high,high,0,1}};
        for(unsigned vertex=0;vertex<4;++vertex)for(unsigned lane=0;lane<4;++lane) {
            put(geometry->vertices.data()+vertex*32+lane*4,std::bit_cast<uint32_t>(positions[vertex][lane]));
            put(geometry->vertices.data()+vertex*32+16+lane*4,std::bit_cast<uint32_t>(coordinates[vertex][lane]));
        }
        return geometry;
    };
    const auto full=rectangle(false),crop=rectangle(true);
    WorldDraw model;model.geometry={full,full,0,6};
    model.targets={colorTarget,0,0,0,depthTarget};model.viewport={0,0,64,64};
    model.material=WorldMaterial::fixed;model.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    model.options.modes.fill(4);model.options.modes[0]=0;
    for(unsigned lane=0;lane<4;++lane)model.constants.vectors[lane][lane]=1;
    model.constants.references[0][2]=10;model.constants.vectors[10]={1,1,1,1};
    put(model.attributes.data()+92,0x01100006);model.attributes[96]=model.attributes[97]=8;
    auto skin=std::make_shared<ColorImage>();skin->width=skin->height=1;
    skin->pixels={192,64,32,255};model.textures[0]=skin;
    auto& sampler=model.samplers[0];sampler.valid=sampler.lodValid=sampler.baseOnly=true;
    sampler.address.fill(2);

    WorldClear scene;scene.targets=model.targets;scene.viewport=model.viewport;
    scene.flags=49;scene.depth=0;scene.stencil=17;
    WorldResolve saved;saved.targets=scene.targets;saved.viewport=scene.viewport;
    saved.rectangle={0,0,64,64};saved.destination={4573,0x4573000,64,64,26,0};
    auto restore=model;restore.geometry={crop,crop,0,6};restore.material=WorldMaterial::post;
    restore.fragmentName="XREngine_TransparentMesh";restore.textures[0].reset();
    restore.textureIds[0]=17;restore.textureObjects[0]=saved.destination;
    // This original source has no live slot1 or dependency on env0.yzw.
    restore.textureObjects[1]={4574,0x4574000,64,64,26,0};
    put(restore.attributes.data()+92,0x01100008);
    restore.attributes[144]=5;restore.attributes[145]=6;
    require(worldFragmentTextureMask(restore.fragmentName,0)==1,
            "Transparent mesh source texture mask differs");
    auto halfFloat=[](uint16_t h) {
        const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);
    };
    const unsigned side=64*scale;
    unsigned cases=0;
    for(float opacity:{0.0f,.25f,.5f,1.0f}) {
        const EngineVector colors[]{{.125f,.5f,.75f,0},{.875f,.25f,.125f,1}};
        scene.color=colors[cases&1];renderer.clear(scene);
        auto right=scene;right.flags=1;right.color=colors[1-(cases&1)];
        right.rectangle=std::array<int32_t,4>{32,0,64,64};renderer.clear(right);
        // Overwrite the same resolved owner each case, with different saved RGB.
        require(renderer.resolve(saved),"Premodel scene snapshot rejected");
        require(renderer.draw(model),"Opaque model fixture rejected");
        const auto opaque=renderer.readSurface(colorTarget,false);
        const auto depth=renderer.readSurface(depthTarget,true);
        const float alpha=1-opacity;restore.fragmentConstants[0]={alpha,1-alpha,97,-3};
        require(renderer.draw(restore),"Original model transparency composite rejected");
        const auto result=renderer.readSurface(colorTarget,false);
        require(result.size()==size_t(side)*side*8 && opaque.size()==result.size(),
                "Transparent mesh output scale differs");
        require(renderer.readSurface(depthTarget,true)==depth,
                "Transparent mesh restoration changed the model depth/stencil");
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
            const size_t offset=(size_t(y)*side+x)*8;
            const bool inside=x>=16*scale && x<48*scale && y>=16*scale && y<48*scale;
            if(!inside) {
                require(!std::memcmp(result.data()+offset,opaque.data()+offset,8),
                        "Transparent mesh changed pixels outside the model screen rectangle");
                continue;
            }
            uint16_t pixel[4]{};std::memcpy(pixel,result.data()+offset,8);
            const auto& source=x<32*scale?scene.color:right.color;
            for(unsigned channel=0;channel<3;++channel) {
                const double expected=source[channel]*alpha+(skin->pixels[channel]/255.0)*opacity;
                require(std::abs(halfFloat(pixel[channel])-expected)<.002,
                        "Transparent mesh RGB differs from saved-scene opacity blending");
            }
            const double expectedAlpha=double(alpha)*alpha+opacity;
            require(std::abs(halfFloat(pixel[3])-expectedAlpha)<.002,
                    "Transparent mesh used sampled alpha instead of the original opacity override");
        }
        const auto kept=result;
        auto missing=restore;missing.textureObjects[0].object=4575;
        require(!renderer.draw(missing) && renderer.readSurface(colorTarget,false)==kept,
                "Missing named premodel scene modified the target or was accepted");
        auto mismatched=restore;mismatched.textureObjects[0].width=32;
        require(!renderer.draw(mismatched) && renderer.readSurface(colorTarget,false)==kept,
                "Mismatched saved-scene dimensions were accepted");
        auto unknown=restore;unknown.fragmentFlags=1;
        require(!renderer.draw(unknown) && renderer.readSurface(colorTarget,false)==kept,
                "Unknown transparent mesh permutation was accepted");
        ++cases;
    }
    std::printf("TransparentMesh%u: %u saved-scene opacity/alpha/crop/depth and resource-generation cases passed.\n",scale,cases);
}
