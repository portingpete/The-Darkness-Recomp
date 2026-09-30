#pragma once

// All stand-in inputs contain readable CPU pixels, including the GPU-only
// ones. Incorrectly broad recovery is therefore observable, rather than
// passing because the framebuffer/depth placeholders happened to be invalid.
template<class BindTexture,class FinishFrame>
static void testWorldSourceRecovery(Memory& memory,const StoredDraw& world,uint32_t device,
                                    uint32_t program,uint32_t name,BindTexture bind,FinishFrame finish) {
    auto* base=memory.base();
    constexpr uint32_t context=0x82A69B00,attributes=context+16896;
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
    auto capture=[&](const char* fragment,unsigned flags,uint16_t allowed) {
        std::strcpy(reinterpret_cast<char*>(base+name),fragment);put32(base,program+16,flags<<8);
        for(unsigned id=0x780;id<0x788;++id)previewPrepareTexture(id);
        previewObserveWorld(base,world);const auto commands=finish();
        require(commands.size()==1 && commands[0].world && commands[0].world->fragmentName==fragment &&
                commands[0].world->fragmentFlags==flags,"World source pass was lost during queue capture");
        const auto draw=commands[0].world;
        const auto mask=worldFragmentTextureMask(fragment,flags);
        require(mask!=0xFFFF && draw->textureMask==mask,"World source fixture did not use a compiled original pass");
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
    std::printf("WorldSourceRecovery: %u water/fog/FX/GUI passes retain authored sources and exclude GPU depth/scene inputs.\n",passes);
}
