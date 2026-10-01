#pragma once

// Verify world/video capture uses the completed texture objects and current
// owned plane uploads. The original binder supplies the draw-time fetch state;
// deleting the CPU cache must recover native 8_8/L8 storage rather than leave
// the TV black or pair current luma with an obsolete chroma generation.
template<class BindTexture,class FinishFrame>
static void testWorldVideoCapture(Memory& memory,const StoredDraw& world,uint32_t device,
                                  uint32_t program,uint32_t name,BindTexture bind,FinishFrame finish) {
    auto* base=memory.base();
    constexpr uint32_t context=0x82A69B00,attributes=context+16896;
    const auto oldTable=memory.read32(context+17964);
    std::array<uint8_t,160> oldAttributes{};
    std::memcpy(oldAttributes.data(),base+attributes,oldAttributes.size());
    const std::vector<uint8_t> oldDevice(base+device,base+device+0x4000);
    const auto scratch=memory.allocate(65536),storage=memory.allocate(65536);
    require(scratch && storage,"World video capture fixture allocation failed");
    struct Restore {
        Memory& memory;uint8_t* base;uint32_t scratch,storage,device,table;
        const std::vector<uint8_t>& savedDevice;const std::array<uint8_t,160>& savedAttributes;
        ~Restore() {
            std::memcpy(base+device,savedDevice.data(),savedDevice.size());
            std::memcpy(base+attributes,savedAttributes.data(),savedAttributes.size());
            put32(base,context+17964,table);
            for(unsigned id:{0x7f0u,0x7f1u})previewPrepareTexture(id);
            memory.release(scratch);memory.release(storage);
        }
    } restore{memory,base,scratch,storage,device,oldTable,oldDevice,oldAttributes};
    std::memset(base+scratch,0,65536);std::memset(base+storage,0,65536);
    std::memset(base+attributes,0,160);put32(base,attributes,program);
    std::strcpy(reinterpret_cast<char*>(base+name),"CMWnd_ModTexture_PaintVideo_YUV2RGB");
    put32(base,program,5);put32(base,program+4,name);put32(base,program+16,0);
    put32(base,context+17964,scratch);
    std::array<uint32_t,2> objects{},images{},planePixels{};
    std::array<std::shared_ptr<const ColorImage>,2> uploaded{};
    for(unsigned slot=0;slot<2;++slot) {
        const unsigned id=0x7f0+slot,w=slot?1:2,h=slot?1:2,bpp=slot?2:1;
        const auto wrapper=scratch+0x3000+slot*256;
        objects[slot]=scratch+0x4000+slot*256;
        images[slot]=scratch+0x5000+slot*256;planePixels[slot]=scratch+0x6000+slot*256;
        put32(base,scratch+(id+1)*4,wrapper);put32(base,wrapper+84,objects[slot]);
        put32(base,wrapper+172,0x10000000);put16(base,attributes+8+slot*2,uint16_t(id));
        const uint32_t fetch[]{2u|(1u<<22),(storage+slot*0x4000)|(slot?10u|64u:2u),
            (w-1)|((h-1)<<13),slot?1u<<10:5u<<10,0u,0x200u};
        for(unsigned word=0;word<6;++word)put32(base,objects[slot]+28+word*4,fetch[word]);
        put32(base,images[slot],0x82097610);put32(base,images[slot]+8,planePixels[slot]);
        put32(base,images[slot]+12,w*h*bpp);put32(base,images[slot]+16,w);put32(base,images[slot]+20,h);
        put32(base,images[slot]+24,w*bpp);put32(base,images[slot]+28,bpp);
        put32(base,images[slot]+32,slot?0x20000:0x2000);put32(base,images[slot]+40,0x810);
        previewPrepareTexture(id);bind(slot,objects[slot]);
    }
    auto update=[&](uint8_t y,uint8_t u,uint8_t v) {
        std::memset(base+planePixels[0],y,4);base[planePixels[1]]=v;base[planePixels[1]+1]=u;
        for(unsigned slot=0;slot<2;++slot) {
            TextureUpload transaction(0x7f0+slot,1,0,1);
            require(transaction.record(base,objects[slot],images[slot],planePixels[slot],0,0,false) &&
                    transaction.complete(0,0),"World video upload transaction rejected");
            uploaded[slot]=transaction.finish(objects[slot]);
            require(uploaded[slot]!=nullptr,"World video upload did not publish a complete plane");
            previewPublishTexture(0x7f0+slot,objects[slot],uploaded[slot]);
        }
        // Independent guest fetch storage: original A8L8 uses 8-in-16 endian
        // swapping and R,R,R,G selectors. 82269810's XDK0x0800014A
        // selects R,R,R,G through82864530's bits18/21/24/27 packing.
        // The observer owns BE V,U bytes; resident storage has U,V. Video
        // recovery normalizes the fetched V,V,V,U to logical U,U,U,V.
        const uint32_t yFetch[]{2u|(1u<<22),storage|2u,1u|(1u<<13),5u<<10,0u,0x200u};
        TextureMipLayout layout;require(!getTextureMipLayout(yFetch,0,0,layout),"World Y storage layout rejected");
        for(unsigned row=0;row<2;++row)std::memset(base+storage+row*layout.rowPitchBytes,y,2);
        base[storage+0x4000]=u;base[storage+0x4001]=v;
    };
    auto capture=[&] {
        previewObserveWorld(base,world);const auto commands=finish();
        require(commands.size()==1 && commands[0].world && commands[0].world->textureMask==3 &&
                commands[0].world->fragmentName=="CMWnd_ModTexture_PaintVideo_YUV2RGB",
                "Original world/TV video pass or two-plane mask was lost");
        const auto draw=commands[0].world;
        for(unsigned slot=0;slot<2;++slot)
            require(draw->textureObjects[slot].object==objects[slot] && draw->textures[slot] &&
                    draw->textures[slot]->valid(),"World video lost a completed binding or owned plane");
        return draw;
    };
    update(81,90,240);const auto first=capture();
    require(first->textures==std::array<std::shared_ptr<const ColorImage>,16>{uploaded[0],uploaded[1]},
            "World video did not retain the current upload snapshots");
    update(41,240,110);const auto second=capture();
    require(second->textures[0]==uploaded[0] && second->textures[1]==uploaded[1] &&
            second->textures[0]!=first->textures[0] && second->textures[1]!=first->textures[1] &&
            first->textures[0]->pixels[0]==81 && first->textures[1]->pixels==std::vector<uint8_t>({90,90,90,240}),
            "Updated world video reused or mutated the previous queued frame");
    ColorImage nativeChroma;
    require(!decodeWorldTextureImage(base,objects[1],nativeChroma) &&
            nativeChroma.pixels==std::vector<uint8_t>({110,110,110,240}),
            "Generic A8L8 fetch10 decoder lost original RRRG/endian semantics");
    for(unsigned id:{0x7f0u,0x7f1u})previewPrepareTexture(id);
    const auto recovered=capture();
    require(recovered->textures[0]->pixels==second->textures[0]->pixels &&
            recovered->textures[1]->pixels==second->textures[1]->pixels,
            "Evicted world video failed L8/A8L8 endian/swizzle recovery");
    const auto reused=capture();
    require(reused->textures[1]==recovered->textures[1] &&
            reused->textures[1]->pixels==std::vector<uint8_t>({240,240,240,110}),
            "Cached video recovery was normalized twice or lost its owned frame");
    std::puts("WorldVideoCapture: original bound objects, live two-plane uploads, retained generations and cache recovery passed.");
}
