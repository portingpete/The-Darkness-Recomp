#pragma once

static void worldVertexValidationContract() {
    struct Format {uint8_t code,bytes;};
    constexpr Format supported[]{{1,4},{2,8},{3,12},{4,16},{9,2},{10,4},{11,6},
        {12,8},{14,4},{15,4},{16,4},{17,4},{18,4},{19,4}};
    unsigned comparisons=0;
    auto agrees=[&](const StoredGeometry& geometry,bool expected) {
        const auto bytes=geometry.vertices;
        std::vector<WorldVertex> decoded;
        require(validateWorldVertices(geometry)==expected,"Packed vertex validation changed acceptance");
        require(decodeWorldVertices(geometry,decoded)==expected,"Vertex decoder disagrees with validation oracle");
        require(geometry.vertices==bytes,"Vertex validation changed owned input");
        ++comparisons;
    };
    // Every semantic, including the non-output slot 11, must consume storage
    // and validate all float lanes. Non-float all-ones data remains legal.
    constexpr uint32_t finiteBits[]{0,0x80000000,1,0x807FFFFF,0x00800000,0x3F800000,0x7F7FFFFF,0xFF7FFFFF};
    constexpr uint32_t nonfiniteBits[]{0x7F800000,0xFF800000,0x7FC00000,0x7F800001,0xFFC12345};
    for(const auto format:supported)for(unsigned slot=0;slot<16;++slot) {
        StoredGeometry g;g.vertexCount=3;g.formats[0]=17;g.formats[slot]=format.code;
        const unsigned offset=slot?4:0;
        g.stride=offset+format.bytes;g.vertices.resize(g.vertexCount*g.stride,0xFF);
        if(format.code<=4) {
            for(unsigned v=0;v<g.vertexCount;++v)for(unsigned lane=0;lane<format.code;++lane)
                put(g.vertices.data()+v*g.stride+offset+lane*4,finiteBits[(v*4+lane)%std::size(finiteBits)]);
            agrees(g,true);
            for(unsigned v=0;v<g.vertexCount;++v)for(unsigned lane=0;lane<format.code;++lane) {
                auto* field=g.vertices.data()+v*g.stride+offset+lane*4;
                for(auto bits:nonfiniteBits) {put(field,bits);agrees(g,false);}
                put(field,0);
            }
        } else agrees(g,true);
    }
    // Vary formats, alignment and finite/nonfinite payloads independently of
    // the validator's span construction; the full decoder is the oracle.
    uint32_t random=0x1289ABCD;
    auto next=[&] {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned trial=0;trial<512;++trial) {
        StoredGeometry g;g.vertexCount=1+next()%17;
        for(unsigned slot=0;slot<16;++slot) {
            if(slot && next()%3==0)continue;
            const auto format=supported[next()%std::size(supported)];
            g.formats[slot]=format.code;g.stride+=format.bytes;
        }
        g.vertices.resize(g.vertexCount*g.stride);
        for(auto& byte:g.vertices)byte=uint8_t(next());
        std::vector<WorldVertex> decoded;
        agrees(g,decodeWorldVertices(g,decoded));
    }
    StoredGeometry valid;valid.vertexCount=3;valid.formats[0]=3;valid.stride=12;valid.vertices.resize(36);
    agrees(valid,true);
    auto bad=valid;bad.vertices.pop_back();agrees(bad,false);
    bad=valid;bad.vertices.push_back(0);agrees(bad,false);
    bad=valid;bad.stride=13;bad.vertices.resize(39);agrees(bad,false);
    bad=valid;bad.stride=UINT32_MAX;agrees(bad,false);
    bad=valid;bad.vertexCount=0;bad.vertices.clear();agrees(bad,false);
    bad=valid;bad.vertexCount=65536;bad.vertices.resize(65536*12);agrees(bad,false);
    bad=valid;bad.formats[0]=0;bad.formats[1]=3;agrees(bad,false);
    for(uint8_t unsupported:{5,6,7,8,13,20,21,22,23,24,25,26,27,255}) {
        bad=valid;bad.formats[11]=unsupported;agrees(bad,false);
    }
    valid.vertexCount=65535;valid.vertices.resize(65535*12);agrees(valid,true);
    put(valid.vertices.data()+valid.vertices.size()-4,0x7FC00000);agrees(valid,false);
    std::printf("Packed vertex validation: %u acceptance comparisons passed.\n",comparisons);
}

static void immediateIndexOwnershipContract() {
    std::vector<uint8_t> memory(256);
    constexpr unsigned indices=128;
    constexpr uint16_t expected[]{1,256,1023,2,513,258};
    for(unsigned i=0;i<std::size(expected);++i) {
        memory[indices+i*2]=uint8_t(expected[i]>>8);
        memory[indices+i*2+1]=uint8_t(expected[i]);
    }
    const auto input=memory;
    auto vertices=std::make_shared<StoredGeometry>();vertices->vertexCount=1024;
    StoredDraw draw;
    require(snapshotImmediateWorldGeometry(memory.data(),0,indices,6,vertices,draw),"Stored-VB immediate indices rejected");
    require(draw.indices->indices==std::vector<uint16_t>(std::begin(expected),std::end(expected)),"Immediate index byte order changed");
    require(memory==input,"Immediate index conversion modified guest memory");
    const auto retained=draw.indices;
    require(snapshotImmediateWorldGeometry(memory.data(),0,indices,6,vertices,draw) && draw.indices==retained,
            "Repeated immediate indices lost cache identity");
    memory[indices+1]=3;
    require(snapshotImmediateWorldGeometry(memory.data(),0,indices,6,vertices,draw) && draw.indices!=retained &&
            retained->indices.front()==1 && draw.indices->indices.front()==3,"Immediate index capture mutated a queued snapshot");
    const auto last=draw.indices;
    memory[indices+10]=4;memory[indices+11]=0;
    ImmediateCaptureReason reason;
    require(!snapshotImmediateWorldGeometry(memory.data(),0,indices,6,vertices,draw,&reason) &&
            reason.stage==ImmediateCaptureReason::Stage::indexOob && reason.detail0==5 && reason.detail1==1024 &&
            draw.indices==last && draw.vertices==vertices && draw.indexCount==6,"OOB immediate index changed output or diagnostic");
}
