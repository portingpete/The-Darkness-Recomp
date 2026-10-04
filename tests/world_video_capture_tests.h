#pragma once

// Synthetic decoder callback for the actual original direct-refresh function.
// Retain its temporary view so fixture cleanup avoids the original guest heap.
static void refreshVideoSource(PPCContext& ctx,uint8_t* base) {
    auto read=[&](uint32_t at){return uint32_t(base[at])<<24|uint32_t(base[at+1])<<16|uint32_t(base[at+2])<<8|base[at+3];};
    const auto source=ctx.r3.u32,request=ctx.r5.u32;
    const auto image=read(read(request)),view=read(image+104);
    require(ctx.r4.u32==7 && read(request+16)==0 && read(request+20)==0 &&
            read(image+16)==1 && read(image+20)==1 && read(image+32)==0x20000,
            "Original video refresh changed the selected A8L8 image or storage reuse");
    require(view==read(source+12) && read(view+24)==read(source+4),
            "Original video refresh selected another texture view");
    put32(base,view+4,read(view+4)+1);
    // WMV8279C63C / Theora8279F8A4 interleave decoder output as V,U.
    base[read(source+8)]=uint8_t(read(source+20));
    base[read(source+8)+1]=uint8_t(read(source+24));
    put32(base,source+16,read(source+16)+1);put32(base,request+24,0x13579bdf);
}

// Verify world/video capture uses the completed texture objects and current
// owned plane uploads. The original binder supplies the draw-time fetch state;
// deleting the CPU cache must recover native 8_8/L8 storage rather than leave
// the TV black or pair current luma with an obsolete chroma generation.
template<class BindTexture,class FinishFrame>
static void testWorldVideoCapture(Memory& memory,const PPCContext& threadContext,const StoredDraw& world,uint32_t device,
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
        // Direct82256008 fills the resident allocation with decoder V,U;
        // unlike initial82257450 it performs no temporary byte-pair swap.
        // The native fetch therefore already exposes logical U,U,U,V.
        const uint32_t yFetch[]{2u|(1u<<22),storage|2u,1u|(1u<<13),5u<<10,0u,0x200u};
        TextureMipLayout layout;require(!getTextureMipLayout(yFetch,0,0,layout),"World Y storage layout rejected");
        for(unsigned row=0;row<2;++row)std::memset(base+storage+row*layout.rowPitchBytes,y,2);
        base[storage+0x4000]=v;base[storage+0x4001]=u;
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
            nativeChroma.pixels==std::vector<uint8_t>({240,240,240,110}),
            "Generic A8L8 fetch10 decoder lost original RRRG/endian semantics");
    for(unsigned id:{0x7f0u,0x7f1u})previewPrepareTexture(id);
    const auto recovered=capture();
    require(recovered->textures[0]->pixels==second->textures[0]->pixels &&
            recovered->textures[1]->pixels==second->textures[1]->pixels,
            "Evicted world video failed L8/A8L8 endian/swizzle recovery");
    const auto reused=capture();
    require(reused->textures[1]==recovered->textures[1] &&
            reused->textures[1]->pixels==std::vector<uint8_t>({240,240,240,110}),
            "Cached video recovery changed its logical U/V view or lost its owned frame");
    {
        // A source-backed warm sample from attraction.wmv frame565. FFmpeg
        // independently decodes uniform Y91/U122/V137 to RGB101/81/74;
        // swapping U/V gives RGB77/87/105. The original shader has slightly
        // different coefficients but must preserve that warm channel order.
        update(91,122,137);const auto before=capture();
        require(before->textures[1]->pixels==std::vector<uint8_t>({122,122,122,137}),
                "Attraction decoder sample lost its independently established U/V order");
        const uint32_t source=scratch+0x7000,vtable=source+256,metadata=source+512,
            view=source+768,output=source+1024,wrapper=scratch+0x3100;
        const uint32_t tls=memory.read32(threadContext.r13.u32),callback=uint32_t(PPC_CODE_BASE);
        const auto oldTlsView=memory.read32(tls+8),oldTlsSize=memory.read32(tls+16);
        auto* oldFunction=PPC_LOOKUP_FUNC(base,callback);
        std::array<uint8_t,1280> oldStack{};std::memcpy(oldStack.data(),base+threadContext.r1.u32-1024,oldStack.size());
        struct RestoreRefresh {
            uint8_t* base;uint32_t tls,oldView,oldSize,callback,stack,csr;PPCFunc* oldFunction;
            const std::array<uint8_t,1280>& bytes;
            ~RestoreRefresh() {
                put32(base,tls+8,oldView);put32(base,tls+16,oldSize);PPC_LOOKUP_FUNC(base,callback)=oldFunction;
                std::memcpy(base+stack,bytes.data(),bytes.size());_mm_setcsr(csr);
            }
        } restoreRefresh{base,tls,oldTlsView,oldTlsSize,callback,threadContext.r1.u32-1024,
                         _mm_getcsr(),oldFunction,oldStack};
        PPC_LOOKUP_FUNC(base,callback)=refreshVideoSource;
        put16(base,wrapper+168,0x7f1);base[wrapper+170]=0;
        // Select the original linear format path (82269868). The resident
        // fetch is linear; the tiled path's XDK0x0800014A would recreate it.
        put32(base,wrapper+172,0x18000000);put32(base,objects[1]+4,1);
        std::array<uint32_t,6> oldFetch{};
        for(unsigned word=0;word<oldFetch.size();++word)oldFetch[word]=memory.read32(objects[1]+28+word*4);
        put32(base,source,vtable);put32(base,vtable+124,callback);put32(base,source+4,objects[1]);
        put32(base,source+8,storage+0x4000);put32(base,source+12,view);put32(base,source+16,0);
        put32(base,source+20,137);put32(base,source+24,122);put32(base,metadata+32,0x20000);
        put32(base,tls+8,view);put32(base,tls+16,48);
        PPCContext refresh;std::memcpy(&refresh,&threadContext,sizeof(refresh));
        refresh.r3.u64=wrapper;refresh.r4.u64=source;refresh.r5.u64=7;refresh.r6.u64=metadata;
        refresh.r9.u64=refresh.r10.u64=1;
        put32(base,refresh.r1.u32+84,1);put32(base,refresh.r1.u32+92,0);put32(base,refresh.r1.u32+100,0);
        put32(base,refresh.r1.u32+116,output);
        sub_82256008(refresh,base);
        require(refresh.r3.u32==1 && refresh.r1.u32==threadContext.r1.u32 &&
                memory.read32(source+16)==1 && memory.read32(output)==0x13579bdf &&
                base[storage+0x4000]==137 && base[storage+0x4001]==122,
                "Original direct video refresh changed decoder V,U storage or skipped its callback");
        for(unsigned word=0;word<oldFetch.size();++word)
            require(memory.read32(objects[1]+28+word*4)==oldFetch[word],
                    "Original direct video refresh unexpectedly recreated its linear A8L8 descriptor");
        const auto after=capture();
        require(after->textures[1]!=before->textures[1] && after->textures[1]->pixels==before->textures[1]->pixels &&
                before->textures[1]->pixels==std::vector<uint8_t>({122,122,122,137}),
                "Actual direct-refresh video recovery reversed U/V or mutated a queued generation");
        previewPrepareTexture(0x7f1);const auto evicted=capture();
        require(evicted->textures[1]!=after->textures[1] && evicted->textures[1]->pixels==before->textures[1]->pixels,
                "Evicted direct-refresh chroma reversed the source attraction colors");
        put32(base,objects[1]+4,1);
    }
    std::puts("WorldVideoCapture: original bound objects, two-plane ownership, actual82256008 direct refresh, attraction color sample and cache recovery passed.");
}
