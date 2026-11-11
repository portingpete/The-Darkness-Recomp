#pragma once

// All stand-in inputs contain readable CPU pixels, including the GPU-only
// ones. Incorrectly broad recovery is therefore observable, rather than
// passing because the framebuffer/depth placeholders happened to be invalid.
template<class BindTexture,class FinishFrame>
static void testWorldSourceRecovery(Memory& memory,const StoredDraw& world,uint32_t device,
                                    uint32_t program,uint32_t name,BindTexture bind,FinishFrame finish) {
    auto* base=memory.base();
    constexpr uint32_t context=0x82A69B00,attributes=context+16896;
    constexpr unsigned pressureFirst=0x790,pressureCount=17;
    const auto oldTable=memory.read32(context+17964);
    std::array<uint8_t,160> oldAttributes{};
    std::memcpy(oldAttributes.data(),base+attributes,oldAttributes.size());
    const std::vector<uint8_t> oldDevice(base+device,base+device+0x4000);
    const auto scratch=memory.allocate(65536),storage=memory.allocate(0x80000);
    require(scratch && storage,"World source recovery fixture allocation failed");
    struct Restore {
        Memory& memory;uint8_t* base;uint32_t scratch,storage,device,table;
        const std::vector<uint8_t>& savedDevice;const std::array<uint8_t,160>& savedAttributes;
        ~Restore() {
            std::memcpy(base+device,savedDevice.data(),savedDevice.size());
            std::memcpy(base+attributes,savedAttributes.data(),savedAttributes.size());
            put32(base,context+17964,table);
            for(unsigned id=0x780;id<0x788;++id)previewPrepareTexture(id);
            for(unsigned i=0;i<pressureCount;++i)previewPrepareTexture(pressureFirst+i);
            setPreviewTextureBudget(256ull*1024*1024);
            memory.release(scratch);memory.release(storage);
        }
    } restore{memory,base,scratch,storage,device,oldTable,oldDevice,oldAttributes};
    std::memset(base+scratch,0,65536);std::memset(base+storage,0,0x80000);
    std::memset(base+attributes,0,160);put32(base,attributes,program);
    put32(base,program,5);put32(base,program+4,name);
    put32(base,context+17964,scratch);
    std::array<uint32_t,8> objects{};
    std::array<std::vector<uint8_t>,8> expected;
    auto setup=[&](bool cubeReflection) {
        for(unsigned slot=0;slot<8;++slot) {
            const unsigned id=0x780+slot;const bool cube=slot==7 || (slot==0 && cubeReflection);
            const auto wrapper=scratch+0x3000+slot*256;objects[slot]=scratch+0x4000+slot*256;
            put32(base,scratch+(id+1)*4,wrapper);put32(base,wrapper+84,objects[slot]);
            put32(base,wrapper+172,0x10000000);put16(base,attributes+8+slot*2,uint16_t(id));
            const uint32_t fetch[]{2u|(1u<<22),(storage+slot*0x8000)|6u,
                3u|(3u<<13)|(cube?5u<<26:0),0xd10u,0u,cube?0x600u:0x200u};
            for(unsigned word=0;word<6;++word)put32(base,objects[slot]+28+word*4,fetch[word]);
            TextureMipLayout layout;require(!getTextureMipLayout(fetch,0,0,layout),"World source fixture layout failed");
            expected[slot].clear();
            for(unsigned face=0;face<(cube?6u:1u);++face)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
                const std::array<uint8_t,4> pixel{uint8_t((x+1)*17+slot*7+face*3),uint8_t(y*31+slot*11+face*5),
                    uint8_t((x+y)*19+slot*13+face*7),uint8_t(255-x*13-y*7-face*9)};
                expected[slot].insert(expected[slot].end(),pixel.begin(),pixel.end());
                std::memcpy(base+layout.allocationAddress+face*layout.faceStrideBytes+y*layout.rowPitchBytes+x*4,pixel.data(),4);
            }
            ColorImage decoded;
            require(!decodeWorldTextureImage(base,objects[slot],decoded) && decoded.pixels==expected[slot],
                    "World source stand-in could not independently decode its pixels");
            bind(slot,objects[slot]);previewPrepareTexture(id);
        }
    };
    auto queue=[&](const char* fragment,unsigned flags,bool cold) {
        std::strcpy(reinterpret_cast<char*>(base+name),fragment);put32(base,program+16,flags<<8);
        if(cold)for(unsigned id=0x780;id<0x788;++id)previewPrepareTexture(id);
        previewObserveWorld(base,world);const auto commands=finish();
        require(commands.size()==1 && commands[0].world && commands[0].world->fragmentName==fragment &&
                commands[0].world->fragmentFlags==flags,"World source pass was lost during queue capture");
        const auto draw=keepWorldDraw(commands,0); // Returned beyond its frame.
        const auto mask=worldFragmentTextureMask(fragment,flags);
        require(mask!=0xFFFF && draw->textureMask==mask,"World source fixture did not use a compiled original pass");
        return draw;
    };
    auto capture=[&](const char* fragment,unsigned flags,uint16_t allowed) {
        const auto draw=queue(fragment,flags,true);
        const auto mask=draw->textureMask;
        for(unsigned slot=0;slot<8;++slot) {
            const bool used=(mask&(1u<<slot))!=0,recover=used && (allowed&(1u<<slot));
            require(bool(draw->textures[slot])==recover,
                    "CPU source recovery omitted authored pixels or decoded a GPU-only/unused source");
            if(used)require(draw->textureObjects[slot].object==objects[slot] && draw->textureIds[slot]==0x780+slot,
                            "Source recovery changed an original completed binding");
            if(recover)require(draw->textures[slot]->valid() && draw->textures[slot]->pixels==expected[slot],
                               "Source recovery lost nonuniform pixels or cube faces");
        }
        return draw;
    };
    unsigned passes=0;
    for(bool cube:{false,true}) {
        setup(cube);
        capture(cube?"VBOp_FP20_CubeWater":"VBOp_FP20_Water",0,uint16_t(0xc6u|(cube?1u:0u)));++passes;
        for(unsigned flags:{0u,1u,3u,5u,7u}) {
            const auto old=capture(cube?"VBOp_FP20_CubeWater2":"VBOp_FP20_Water2",flags,uint16_t(0xc6u|(cube?1u:0u)));++passes;
            require(!old->textures[4] && (cube || !old->textures[0]),
                    "Water scene refraction or planar reflection recovered CPU storage");
        }
    }
    setup(false);
    capture("WModel_FXRenderSurface",0,2);++passes;
    capture("WModel_FXHeatHazeMask",0,6);++passes;
    capture("WModel_FXBlackHole",0,6);++passes;
    capture("GUIRGB2Grey",0,1);++passes;

    setup(false);
    auto voice=[&](bool cold) {return queue("XREngine_RadialBlurInvert",0,cold);};
    auto verifyVoice=[&](const auto& draw,uint16_t authoredMask) {
        require(draw->textureMask==0x0f,"Voice fixture did not retain scene/mask/noise bindings");
        require(draw->textureObjects[0].object==objects[0] && draw->textureIds[0]==0x780 && !draw->textures[0],
                "Voice recovery decoded readable CPU storage for its GPU scene input");
        for(unsigned slot=1;slot<4;++slot) {
            require(draw->textureObjects[slot].object==objects[slot] && draw->textureIds[slot]==0x780+slot,
                    "Voice source recovery changed a completed original binding");
            const bool authored=(authoredMask&(1u<<slot))!=0;
            require(bool(draw->textures[slot])==authored,"Voice source recovery omitted or revived an authored input");
            if(authored)require(draw->textures[slot]->valid() && draw->textures[slot]->authoredMips &&
                    draw->textures[slot]->width==4 && draw->textures[slot]->height==4 &&
                    draw->textures[slot]->pixels==expected[slot],
                    "Voice source recovery changed the nonuniform authored pixels");
        }
        for(unsigned slot=4;slot<draw->textures.size();++slot)
            require(!draw->textures[slot],"Voice recovery decoded an unused texture slot");
    };
    // Start with completed uploads so the first regression assertion exercises
    // a working draw, a real budget trim, then unchanged original bindings.
    setPreviewTextureBudget(320ull*1024*1024);
    for(unsigned slot=1;slot<4;++slot) {
        auto image=std::make_shared<ColorImage>();
        require(!decodeWorldTextureImage(base,objects[slot],*image),"Voice upload fixture could not decode authored pixels");
        previewPublishTexture(0x780+slot,objects[slot],std::move(image));
    }
    const auto owned=voice(false);verifyVoice(owned,0x0e);++passes;
    auto pressure=std::make_shared<ColorImage>();pressure->width=pressure->height=2048;
    pressure->pixels.resize(2048ull*2048*4,37);
    for(unsigned i=0;i<pressureCount;++i)previewPublishTexture(pressureFirst+i,scratch+i+1,pressure);
    for(unsigned i=0;i<pressureCount;++i)
        require(previewCapturedTexture(pressureFirst+i,scratch+i+1)==pressure,"Voice pressure fixture did not fit the grown budget");
    for(unsigned slot=1;slot<4;++slot)
        require(previewCapturedTexture(0x780+slot,objects[slot])==owned->textures[slot],
                "Voice pressure evicted a source before the budget shrink");
    setPreviewTextureBudget(256ull*1024*1024);
    for(unsigned slot=1;slot<4;++slot)
        require(!previewCapturedTexture(0x780+slot,objects[slot]),"Voice budget shrink did not evict the older authored sources");
    const auto recovered=voice(false);++passes;
    require(recovered->textures[1] && recovered->textures[2] && recovered->textures[3],
            "Voice masks/noise did not recover after decoded-cache budget shrink");
    verifyVoice(recovered,0x0e);verifyVoice(owned,0x0e);
    for(unsigned slot=1;slot<4;++slot) {
        require(recovered->textures[slot]!=owned->textures[slot] &&
                previewCapturedTexture(0x780+slot,objects[slot])==recovered->textures[slot],
                "Voice recapture reused an evicted generation or failed to cache the reconstructed source");
    }
    for(unsigned i=0;i<pressureCount;++i)previewPrepareTexture(pressureFirst+i);
    pressure.reset();

    const auto cold=voice(true);verifyVoice(cold,0x0e);verifyVoice(owned,0x0e);++passes;
    // A valid binding with an unsupported encoding must not publish a partial
    // decode or borrow the noise image from an earlier completed draw.
    const auto noiseFetch=memory.read32(objects[3]+32);
    put32(base,objects[3]+32,(noiseFetch&~63u)|63u);bind(3,objects[3]);previewPrepareTexture(0x783);
    ColorImage invalid;
    require(decodeWorldTextureImage(base,objects[3],invalid)!=nullptr,"Invalid voice-noise encoding unexpectedly decoded");
    const auto failed=voice(false);verifyVoice(failed,0x06);++passes;
    require(!previewCapturedTexture(0x783,objects[3]),"Failed voice-noise decode entered the CPU cache");
    verifyVoice(cold,0x0e);
    put32(base,objects[3]+32,noiseFetch);bind(3,objects[3]);
    const auto repaired=voice(false);verifyVoice(repaired,0x0e);++passes;

    // Death can leave the old IDs and fetch words behind while explicitly
    // unbinding the mask/noise objects. Cached pixels must remain unavailable.
    for(unsigned slot=1;slot<4;++slot)bind(slot,0);
    const auto death=voice(false);++passes;
    require(death->textureMask==0x0f && death->textureObjects[0].object==objects[0] && !death->textures[0],
            "Unbound voice pass changed or recovered its GPU scene input");
    for(unsigned slot=1;slot<4;++slot)
        require(death->textureIds[slot]==0x780+slot && !death->textureObjects[slot].object && !death->textures[slot] &&
                previewCapturedTexture(0x780+slot,objects[slot])==repaired->textures[slot],
                "Unbound death masks revived cached images or discarded their owned generation");
    verifyVoice(repaired,0x0e);verifyVoice(owned,0x0e);
    std::printf("WorldSourceRecovery: %u water/fog/FX/GUI/voice passes retain authored pixels, recover after budget trim, and exclude GPU scene or unbound inputs.\n",passes);
}
