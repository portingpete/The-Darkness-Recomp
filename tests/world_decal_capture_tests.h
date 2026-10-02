#pragma once

// Exercise the original fragment/cache binder and descriptor/index builders,
// then the real producer queue. These are authored decal inputs: the three
// projector maps are cubes, and no scene/depth resolve may stand in for them.
template<class BindTexture,class FinishFrame>
static void testWorldDecalCapture(Memory& memory,const PPCContext& initial,uint32_t device,
                                 BindTexture bind,FinishFrame finish) {
    auto* base=memory.base();
    constexpr uint32_t context=0x82A69B00,attributes=context+16896;
    const std::vector<uint8_t> oldContext(base+context,base+context+0x4400);
    const std::vector<uint8_t> oldDevice(base+device,base+device+0x4000);
    const auto oldTable=memory.read32(context+17964),oldDebugConstants=memory.read32(context+18116);
    std::array<uint8_t,108> oldModeFlags{};std::array<uint8_t,27> oldReservations{};
    std::memcpy(oldModeFlags.data(),base+0x82A5CD88,oldModeFlags.size());
    std::memcpy(oldReservations.data(),base+0x82A5CDF4,oldReservations.size());
    const auto scratch=memory.allocate(65536),storage=memory.allocate(0x40000);
    require(scratch && storage,"Decal capture fixture allocation failed");
    struct Restore {
        Memory& memory;uint8_t* base;uint32_t scratch,storage,device,table,debugConstants;
        const std::vector<uint8_t>& savedContext;const std::vector<uint8_t>& savedDevice;
        const std::array<uint8_t,108>& modeFlags;const std::array<uint8_t,27>& reservations;
        ~Restore() {
            std::memcpy(base+context,savedContext.data(),savedContext.size());
            std::memcpy(base+device,savedDevice.data(),savedDevice.size());
            put32(base,context+17964,table);put32(base,context+18116,debugConstants);
            std::memcpy(base+0x82A5CD88,modeFlags.data(),modeFlags.size());
            std::memcpy(base+0x82A5CDF4,reservations.data(),reservations.size());
            for(unsigned id=0x7a0;id<0x7a8;++id)previewPrepareTexture(id);
            memory.release(scratch);memory.release(storage);
        }
    } restore{memory,base,scratch,storage,device,oldTable,oldDebugConstants,oldContext,oldDevice,oldModeFlags,oldReservations};
    std::memset(base+scratch,0,65536);std::memset(base+storage,0,0x40000);
    std::memset(base+attributes,0,160);base[attributes+97]=8;
    put32(base,context+15748,device);put32(base,context+17964,scratch);
    put32(base,context+18116,0); // Original debug-constant override is inactive.
    const auto program=scratch+0x6000,name=scratch+0x6100,env=scratch+0x6200;
    const auto cache=scratch+0x6500,shader=scratch+0x6600;
    constexpr uint32_t sourceId=17;
    std::strcpy(reinterpret_cast<char*>(base+name),"XRShader_FP20_Decal");
    put32(base,attributes,program);put32(base,program,5);put32(base,program+4,name);
    put32(base,program+8,sourceId);put32(base,program+12,env);
    put32(base,context+17200,(cache+20)|1);put32(base,cache+8,shader);
    put32(base,context+17196,0);put32(base,device+12684,0);
    // The original native shader object has no sampler patch table. Its
    // actual binding is still completed by 82868410 during the cache hit.
    put32(base,shader+60,0);
    std::array<uint32_t,8> objects{};
    std::array<std::vector<uint8_t>,8> expectedPixels;
    for(unsigned slot=0;slot<8;++slot) {
        const unsigned id=0x7a0+slot;const bool cube=slot>=3 && slot<=5;
        const auto wrapper=scratch+0x3000+slot*256;objects[slot]=scratch+0x4000+slot*256;
        put32(base,scratch+(id+1)*4,wrapper);put32(base,wrapper+84,objects[slot]);
        put32(base,wrapper+172,0x10000000);put16(base,attributes+8+slot*2,uint16_t(id));
        const uint32_t fetch[]{2u|(1u<<22),(storage+slot*0x8000)|6u,
            3u|(3u<<13)|(cube?5u<<26:0),0xd10u,0u,cube?0x600u:0x200u};
        for(unsigned word=0;word<6;++word)put32(base,objects[slot]+28+word*4,fetch[word]);
        TextureMipLayout layout;require(!getTextureMipLayout(fetch,0,0,layout),"Decal source layout rejected");
        for(unsigned face=0;face<(cube?6u:1u);++face)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
            const std::array<uint8_t,4> pixel{uint8_t(19+x*29+slot*7+face*3),uint8_t(13+y*31+slot*11+face*5),
                uint8_t(7+(x+y)*19+slot*13+face*7),uint8_t(255-x*13-y*7-face*9)};
            expectedPixels[slot].insert(expectedPixels[slot].end(),pixel.begin(),pixel.end());
            std::memcpy(base+layout.allocationAddress+face*layout.faceStrideBytes+y*layout.rowPitchBytes+x*4,pixel.data(),4);
        }
        bind(slot,objects[slot]);previewPrepareTexture(id);
    }
    // Original decoded list geometry owns positions, UVs, tangent streams and
    // normal data. Stage1 will generate world position from position, not UV1.
    const auto cpu=scratch+0x7000,positions=scratch+0x7100,uv=scratch+0x7200;
    const auto tangentU=scratch+0x7300,tangentV=scratch+0x7400,normals=scratch+0x7500;
    const EngineVector expectedPositions[]{{1,2,.25f,1},{2,-1,.5f,1},{-.5f,3,.75f,1}};
    put16(base,cpu,3);put32(base,cpu+4,positions);put32(base,cpu+8,uv);base[cpu+40]=2;
    put32(base,cpu+16,tangentU);put32(base,cpu+20,tangentV);base[cpu+42]=base[cpu+43]=3;
    put32(base,cpu+48,normals);
    for(unsigned vertex=0;vertex<3;++vertex) {
        for(unsigned lane=0;lane<3;++lane) {
            putFloat(base,positions+vertex*12+lane*4,expectedPositions[vertex][lane]);
            putFloat(base,tangentU+vertex*12+lane*4,lane==0?1.f:0.f);
            putFloat(base,tangentV+vertex*12+lane*4,lane==1?1.f:0.f);
            putFloat(base,normals+vertex*12+lane*4,lane==2?1.f:0.f);
        }
        putFloat(base,uv+vertex*8,.125f+vertex*.25f);putFloat(base,uv+vertex*8+4,.875f-vertex*.25f);
    }
    const auto cursor=scratch+0x7600,packed=scratch+0x7700,indexOutput=scratch+0x7800,count=cursor+32;
    put32(base,cursor,packed);put32(base,cursor+12,packed+10);put32(base,count,8192);
    put16(base,packed,0x0501);put16(base,packed+2,1);
    for(unsigned vertex=0;vertex<3;++vertex)put16(base,packed+4+vertex*2,uint16_t(vertex));
    PPCContext decoder;std::memcpy(&decoder,&initial,sizeof decoder);
    decoder.r3.u32=cursor;decoder.r4.u32=indexOutput;decoder.r5.u32=count;decoder.lr=0x8225E1A0;
    __imp__sub_8225F320(decoder,base);
    require(memory.read32(count)==3,"Original decal list decoder lost its triangle");
    StoredDraw world;
    require(snapshotImmediateWorldGeometry(base,cpu,indexOutput,3,{},world) && world.indices->indices==std::vector<uint16_t>({0,1,2}),
            "Decoded original decal geometry was not retained");
    std::vector<WorldVertex> vertices;
    require(decodeWorldVertices(*world.vertices,vertices) && vertices.size()==3,"Decal vertex streams did not decode");
    for(unsigned vertex=0;vertex<3;++vertex)
        require(vertices[vertex].position==expectedPositions[vertex] &&
                vertices[vertex].tex[0]==EngineVector{.125f+vertex*.25f,.875f-vertex*.25f,0,1} &&
                vertices[vertex].tex[2]==EngineVector{1,0,0,1} && vertices[vertex].tex[3]==EngineVector{0,1,0,1} &&
                vertices[vertex].normal==EngineVector{0,0,1,1},"Original decal geometry lost positions, mapping or basis streams");
    PPCContext initialize;std::memcpy(&initialize,&initial,sizeof initialize);__imp__sub_8223AFE8(initialize,base);
    const auto descriptor=scratch+0x7900,matrices=scratch+0x7a00;
    const EngineVector worldRows[]{{2,-3,.5f,7},{-1,4,2,-5},{3,.25f,-2,11}};
    auto prepareVertex=[&](bool trimesh) {
        EngineVertexDescriptor original;original.flags=0x01000000;
        original.parameters[1][0]=12;original.parameters[4][0]=15;original.parameters[5][0]=16;
        const auto initialBytes=encodeEngineVertexDescriptor(original);
        std::memcpy(base+descriptor,initialBytes.data(),initialBytes.size());
        const std::array<uint8_t,8> modes{uint8_t(trimesh?4:0),10,4,4,20,uint8_t(trimesh?4:18),4,4};
        std::memcpy(base+attributes+48,modes.data(),modes.size());
        PPCContext builder;std::memcpy(&builder,&initial,sizeof builder);
        builder.r3.u32=descriptor;builder.r4.u32=attributes;builder.r5.u32=matrices;builder.r6.u32=255;
        __imp__sub_8224DAE0(builder,base);
        std::array<uint8_t,80> bytes{};std::memcpy(bytes.data(),base+descriptor,bytes.size());
        EngineVertexBindingSnapshot binding;binding.deviceAddress=device;binding.descriptor=decodeEngineVertexDescriptor(bytes);
        for(unsigned word=0;word<5;++word)binding.key[word+1]=memory.read32(descriptor+word*4);
        require(binding.descriptor.modes==modes && binding.descriptor.parameters[1][0]==12,
                "Original decal descriptor builder changed its world-position mode or constant selector");
        auto row=[&](unsigned vector,const EngineVector& value) {
            for(unsigned lane=0;lane<4;++lane)putFloat(binding.constantBytes.data(),vector*16+lane*4,value[lane]);
        };
        for(unsigned r=0;r<4;++r){EngineVector identity{};identity[r]=1;row(r,identity);}
        row(8,{0,1,.5f,1});row(10,{1,1,1,1});
        for(unsigned r=0;r<3;++r)row(12+r,worldRows[r]);
        row(15,{4,5,6,1});row(16,{1,0,0,0});row(17,{0,1,0,0});row(18,{0,0,1,0});
        world.vertexBindings=binding;
    };
    std::array<std::shared_ptr<const WorldDraw>,2> retained;
    std::array<std::array<EngineVector,64>,2> expectedConstants{};
    for(unsigned generation=0;generation<2;++generation) {
        const unsigned flags=generation?127:126,vectors=generation?34:30;
        put32(base,program+16,(flags<<8)|vectors);put32(base,cache+12,sourceId*33+flags);
        for(unsigned vector=0;vector<36;++vector)for(unsigned lane=0;lane<4;++lane) {
            const float value=float(100*generation+vector*4+lane+1)/8.f;
            putFloat(base,env+vector*16+lane*4,value);
            if(vector<vectors)expectedConstants[generation][vector][lane]=value;
        }
        PPCContext fragment;std::memcpy(&fragment,&initial,sizeof fragment);
        fragment.r3.u32=program;fragment.r4.u32=attributes+64;__imp__sub_822478C0(fragment,base);
        require(memory.read32(context+17196)==cache && memory.read32(device+12684)==shader &&
                !std::memcmp(base+device+6016,base+env,((vectors+3)&~3u)*16),
                "Original decal binder failed cache selection or the completed rounded constant upload");
        prepareVertex(generation!=0);
        // The program's mutable source is already reusable before observation;
        // only the original completed device upload is the draw-time source.
        std::memset(base+env,0xFF,36*16);
        previewObserveWorld(base,world);const auto commands=finish();
        require(commands.size()==1 && commands[0].world,"Original lit/projector decal capture was omitted or duplicated");
        retained[generation]=commands[0].world;const auto& draw=*retained[generation];
        require(draw.fragmentName=="XRShader_FP20_Decal" && draw.fragmentFlags==flags && draw.textureMask==0x3f &&
                draw.fragmentConstants==expectedConstants[generation],"Decal flags or uploaded lighting/projector constants were lost");
        require(draw.options.modes[1]==10 && draw.constants.references[2][2]==12 &&
                draw.options.modes[4]==20 && draw.options.modes[5]==(generation?4:18),
                "Decal capture lost its original world-position, eye or basis program");
        for(unsigned r=0;r<3;++r)require(draw.constants.vectors[12+r]==worldRows[r],"Decal world-position transform was omitted");
        for(unsigned slot=0;slot<8;++slot) {
            if(slot<6)require(draw.textureObjects[slot].object==objects[slot] && draw.textureIds[slot]==0x7a0+slot &&
                draw.textures[slot] && draw.textures[slot]->faces==(slot<3?1u:6u) &&
                draw.textures[slot]->pixels==expectedPixels[slot],"Decal lost an authored 2D source or projector cube face");
            else require(!draw.textures[slot] && !draw.textureObjects[slot].object,"Decal captured an unused readable source");
        }
    }
    std::memset(base+device+6016,0,36*16);std::memset(base+positions,0,36);std::memset(base+storage,0,0x40000);
    world.vertexBindings->constantBytes.fill(0);
    for(unsigned id=0x7a0;id<0x7a8;++id)previewPrepareTexture(id);
    require(retained[0]->fragmentConstants==expectedConstants[0] && retained[1]->fragmentConstants==expectedConstants[1] &&
            retained[0]->constants.vectors[12]==worldRows[0] && retained[1]->textures[5]->pixels==expectedPixels[5] &&
            decodeWorldVertices(*retained[0]->geometry.vertices,vertices) && vertices[0].position==expectedPositions[0],
            "Guest reuse or cache eviction mutated a queued blood-decal draw");
    std::puts("WorldDecalCapture: original fragment/index/descriptor builders, env0..33, world-position mode10, authored2D/projector cubes and immutable queue capture passed.");
}
