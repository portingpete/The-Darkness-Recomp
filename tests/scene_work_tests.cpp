#include "renderer/engine/scene_work.h"
#include "renderer/engine/world_mesh.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string_view>
#include <xmmintrin.h>
using namespace DarkRecomp::Native;
using DarkRecomp::ColorImage;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static void put(uint8_t* p,uint32_t word) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(word>>(24-i*8));}
static void little(uint8_t* p,uint32_t word) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(word>>(i*8));}

static void worldDescriptorContract() {
    uint32_t random=0x41D38729;
    auto next=[&] {random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    unsigned comparisons=0;
    const auto csr=_mm_getcsr();
    for(unsigned trial=0;trial<128;++trial) {
        EngineVertexBindingSnapshot binding;
        auto& d=binding.descriptor;
        d.coordinateMapping=next();d.flags=0x01000000|(next()&0x0080FFFF);
        d.declarationFlags=next();d.textureReservation=uint16_t(next());d.reserved22=uint16_t(next());
        d.conversionBase=uint8_t(next());d.positionConversion=uint8_t(next());
        d.palette=uint8_t(next());d.color=uint8_t(next());d.reserved76=next();
        for(unsigned stage=0;stage<8;++stage) {
            d.modes[stage]=next()&1?0:4;
            d.conversions[stage]=uint8_t(next()%240);d.matrices[stage]=uint8_t(next()%240);
            for(auto& parameter:d.parameters[stage])parameter=uint8_t(next()%240);
        }
        if(trial%3==2)d.modes={0,10,4,16,9,17,4,4};
        const auto encoded=encodeEngineVertexDescriptor(d);
        binding.key[0]=next(); // Existing check deliberately excludes key0.
        for(unsigned word=0;word<5;++word) {
            binding.key[word+1]=0;
            for(unsigned byte=0;byte<4;++byte)binding.key[word+1]=(binding.key[word+1]<<8)|encoded[word*4+byte];
        }
        const auto before=binding;
        WorldVertexOptions options;WorldVertexConstants constants;
        require(prepareWorldVertexProgram(binding,options,constants),"Encoded five-word descriptor rejected");
        WorldVertexOptions fallbackOptions;WorldVertexConstants fallbackConstants;
        require(prepareWorldVertexProgramWithGeometry(binding,{},fallbackOptions,fallbackConstants) &&
            options==fallbackOptions && constants.vectors==fallbackConstants.vectors &&
            constants.references==fallbackConstants.references,"Strict and fallback descriptor checks disagree");
        ++comparisons;
        WorldVertexOptions sentinelOptions;sentinelOptions.weights=7;sentinelOptions.normal=true;sentinelOptions.modes.fill(255);
        WorldVertexConstants sentinelConstants;sentinelConstants.vectors[37][2]=123.25f;
        sentinelConstants.references[4][1]=0xDEADBEEF;
        for(unsigned word=0;word<5;++word)for(unsigned bit:{0u,7u,15u,23u,31u}) {
            auto bad=binding;bad.key[word+1]^=1u<<bit;
            // Include a later mismatch while retaining the earlier bad word.
            if(word<4)bad.key[5]^=0x40000000;
            options=sentinelOptions;constants=sentinelConstants;
            require(!prepareWorldVertexProgram(bad,options,constants) && options==sentinelOptions &&
                constants.vectors==sentinelConstants.vectors && constants.references==sentinelConstants.references,
                "Mismatched five-word descriptor changed published strict output");
            require(!prepareWorldVertexProgramWithGeometry(bad,{},options,constants) && options==sentinelOptions &&
                constants.vectors==sentinelConstants.vectors && constants.references==sentinelConstants.references,
                "Mismatched five-word descriptor changed published fallback output");
            ++comparisons;
        }
        require(binding==before && _mm_getcsr()==csr,"Descriptor comparison changed source or floating-point state");
    }
    std::printf("World descriptor contract passed: %u encoded key comparisons; all five words, BE mode packing, ignored tail/key0, multiple mismatches and output ownership.\n",comparisons);
}

static void executorContract() {
    SceneWorkPool pool(3);
    require(pool.workers()==3,"Test pool could not create workers");
    std::mutex mutex;std::condition_variable arrived;
    unsigned participants=0;std::set<DWORD> threads;
    require(pool.run(4,1,[&](size_t first,size_t end) {
        std::unique_lock lock(mutex);threads.insert(GetCurrentThreadId());++participants;arrived.notify_all();
        return first+1==end && arrived.wait_for(lock,std::chrono::seconds(5),[&]{return participants==4;});
    }),"Ranges did not execute concurrently");
    require(threads.size()==4,"Pool failed to engage three workers and the caller");
    std::vector<std::atomic<unsigned>> hits(4099);
    require(pool.run(hits.size(),127,[&](size_t first,size_t end) {
        return pool.run(end-first,3,[&](size_t begin,size_t stop) {
            for(size_t n=begin;n<stop;++n)++hits[first+n];return true;
        });
    }),"Recursive work deadlocked or failed");
    for(auto& n:hits)require(n==1,"Range overlap, omission, or tail error");
    require(!pool.run(10000,32,[](size_t,size_t){throw std::runtime_error("expected");return true;}),"Worker exception was lost");
    require(pool.run(10000,32,[](size_t,size_t){return true;}),"Failure poisoned the next batch");
    std::atomic<unsigned> completed=0;
    auto concurrent=[&] {
        for(unsigned repeat=0;repeat<100;++repeat)
            if(pool.run(1027,31,[](size_t first,size_t end){return first<end;}))++completed;
    };
    std::thread other(concurrent);concurrent();other.join();
    require(completed==200,"Concurrent producers lost work");
    SceneWorkPool serial(0);unsigned calls=0;
    require(serial.run(100,1,[&](size_t a,size_t b){++calls;return a==0 && b==100;}),"Serial fallback changed range");
    require(serial.run(0,1,[&](size_t,size_t){++calls;return false;}) && calls==1,"Empty range called callback");
}
static void geometryContract() {
    StoredGeometry g;g.vertexCount=8193;g.stride=56;
    g.formats[0]=3;g.formats[1]=15;g.formats[9]=14;g.formats[10]=18;
    g.formats[12]=12;g.formats[13]=4;g.formats[14]=17;g.formats[15]=16;
    g.vertices.resize(size_t(g.vertexCount)*g.stride);
    uint32_t random=0x13241342;
    for(auto& byte:g.vertices){random^=random<<13;random^=random>>17;random^=random<<5;byte=uint8_t(random);}
    for(unsigned v=0;v<g.vertexCount;++v) {
        auto* p=g.vertices.data()+size_t(v)*g.stride;
        for(unsigned n=0;n<3;++n)put(p+n*4,std::bit_cast<uint32_t>(float(int(v)-1024)/7.0f));
        for(unsigned n=0;n<4;++n)put(p+32+n*4,std::bit_cast<uint32_t>(float(n)/3.0f));
    }
    const auto original=g.vertices;const auto csr=_mm_getcsr();
    for(unsigned rounding:{0u,0x2000u,0x4000u,0x6000u}) {
        _mm_setcsr((csr&~0x6000u)|rounding);
        std::vector<WorldVertex> parallel;
        require(decodeWorldVertices(g,parallel),"Parallel geometry decode failed");
        StoredGeometry one=g;one.vertexCount=1;one.vertices.resize(g.stride);
        for(unsigned v=0;v<g.vertexCount;++v) {
            std::memcpy(one.vertices.data(),g.vertices.data()+size_t(v)*g.stride,g.stride);
            std::vector<WorldVertex> expected;
            require(decodeWorldVertices(one,expected) && !std::memcmp(&expected[0],&parallel[v],sizeof(WorldVertex)),
                "Parallel geometry differs from per-vertex serial decode");
        }
        require((_mm_getcsr()&0x6000u)==rounding,"Worker changed caller rounding mode");
    }
    _mm_setcsr(csr);
    require(g.vertices==original,"Workers changed source geometry");
    std::vector<WorldVertex> before(1);before[0].position[0]=123;
    auto output=before;put(g.vertices.data()+size_t(g.vertexCount-1)*g.stride,0x7FC00000);
    require(!decodeWorldVertices(g,output) && output.size()==1 && !std::memcmp(output.data(),before.data(),sizeof(WorldVertex)),
        "Failed parallel decode published partial output");
    g.vertexCount=65535;g.vertices.resize(size_t(g.vertexCount)*g.stride,0);
    require(!validateWorldVertices(g),"Parallel validator missed nonfinite input");
    put(g.vertices.data()+size_t(8192)*g.stride,0);
    require(validateWorldVertices(g),"Parallel validator rejected valid input");
}
static void immediateStreamContract() {
    constexpr uint32_t descriptor=64,firstStream=256;
    for(unsigned count:{3u,127u,128u,2049u})for(unsigned layout=0;layout<3;++layout) {
        std::array<uint32_t,16> pointers{};
        std::array<uint8_t,16> widths{},formats{};
        widths[0]=12;formats[0]=3;
        if(layout) {
            for(unsigned slot=1;slot<=8;++slot)if(layout==2 || slot%3) {
                formats[slot]=uint8_t(1+(slot%4));widths[slot]=formats[slot]*4;
            }
            if(layout==2) {widths[9]=12;formats[9]=3;}
            widths[10]=4;formats[10]=18;
        }
        uint32_t next=firstStream,stride=0;
        for(unsigned slot=0;slot<16;++slot)if(widths[slot]) {
            pointers[slot]=next;next+=count*widths[slot];stride+=widths[slot];
        }
        const uint32_t indices=next;
        std::vector<uint8_t> source(indices+6),expected(size_t(count)*stride);
        auto* base=source.data();
        put(base+descriptor,count<<16);put(base+descriptor+4,pointers[0]);
        for(unsigned slot=1;slot<=8;++slot) {
            put(base+descriptor+4+slot*4,pointers[slot]);base[descriptor+39+slot]=formats[slot];
        }
        put(base+descriptor+48,pointers[9]);put(base+descriptor+52,pointers[10]);
        base[indices+3]=1;base[indices+5]=2;
        uint32_t random=0x29E637A1;
        for(unsigned vertex=0;vertex<count;++vertex) {
            size_t offset=size_t(vertex)*stride;
            for(unsigned slot=0;slot<16;++slot)if(widths[slot]) {
                auto* field=base+pointers[slot]+size_t(vertex)*widths[slot];
                for(unsigned lane=0;lane<widths[slot];lane+=4) {
                    random^=random<<13;random^=random>>17;random^=random<<5;
                    put(field+lane,slot==10?random:0x3E000000u|(random&0x807FFFFFu));
                }
                std::memcpy(expected.data()+offset,field,widths[slot]);offset+=widths[slot];
            }
        }
        const auto before=source;
        StoredDraw draw;
        require(snapshotImmediateWorldGeometry(base,descriptor,indices,3,{},draw) && draw.vertices && draw.indices,
                "Immediate stream snapshot failed");
        require(draw.vertices->vertices==expected && draw.vertices->formats==formats && draw.vertices->stride==stride &&
                draw.vertices->vertexCount==count && draw.indices->indices==std::vector<uint16_t>{0,1,2},
                "Immediate stream capture changed bytes, format order, stride or indices");
        require(source==before,"Immediate stream capture changed guest bytes");
        const auto retained=draw.vertices;
        put(base+pointers[0],0x3F800000);
        require(snapshotImmediateWorldGeometry(base,descriptor,indices,3,{},draw) &&
                draw.vertices!=retained && retained->vertices==expected,
                "Changed immediate streams mutated a retained snapshot");
        if(layout) {
            const auto vertices=draw.vertices,triangles=draw.indices;
            base[descriptor+40]=5;put(base+descriptor+8,pointers[1]);
            require(!snapshotImmediateWorldGeometry(base,descriptor,indices,3,{},draw) &&
                    draw.vertices==vertices && draw.indices==triangles,
                    "Invalid immediate components published partial output");
        }
    }
}
static void immediateRetentionContract() {
    constexpr uint32_t descriptor=64,positions=256,indexCount=66,indices=positions+1366*12;
    std::vector<uint8_t> source(indices+indexCount*2);
    auto* base=source.data();
    put(base+descriptor,32u<<16);put(base+descriptor+4,positions);
    put(base+positions,0x3F000000);
    std::vector<uint16_t> expectedIndices(indexCount);
    for(unsigned index=0;index<indexCount;++index)
        base[indices+index*2+1]=uint8_t(expectedIndices[index]=index%3);
    StoredDraw first,repeated,collision;
    require(snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},first) &&
            snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},repeated) &&
            first.vertices==repeated.vertices && first.indices==repeated.indices,
            "Alternating vertex/index retention failed to reuse exact owned snapshots");
    const auto firstBytes=first.vertices->vertices;
    auto retainedVertices=first.vertices,retainedIndices=first.indices;
    std::weak_ptr<const StoredGeometry> oldVertices=first.vertices,oldIndices=first.indices;
    // FNV's 8-byte chunk multiply cannot propagate upper bits into the low
    // nine bits used for the cache index. This BE mantissa-bit mutation has
    // the same bucket as the first capture and different complete bytes.
    put(base+positions,0x3F000001);
    require(snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},collision) &&
            collision.vertices!=retainedVertices && collision.indices==retainedIndices &&
            retainedVertices->vertices==firstBytes && collision.vertices->vertices!=firstBytes,
            "A hash collision aliased changed vertices or disturbed the separate index role");
    first=StoredDraw{};repeated=StoredDraw{};retainedVertices.reset();
    require(oldVertices.expired(),"An evicted vertex snapshot remained owned outside the bounded cache");

    // Changing the second native uint16 modifies upper bits of its 8-byte
    // hash chunk, preserving this index list's bucket and changing its bytes.
    base[indices+3]=2;
    StoredDraw larger;
    auto changedIndices=expectedIndices;changedIndices[1]=2;
    require(snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},larger) &&
            larger.vertices==collision.vertices && larger.indices!=retainedIndices &&
            larger.indices->indices==changedIndices && retainedIndices->indices==expectedIndices,
            "An index collision reused a different list or changed a queued snapshot");
    collision=StoredDraw{};retainedIndices.reset();
    require(oldIndices.expired(),"An evicted index snapshot remained owned outside the bounded cache");
    const auto validVertices=larger.vertices,validIndices=larger.indices;
    put(base+positions,0x7F800001); // Referenced signaling NaN cannot recover.
    require(!snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},larger) &&
            larger.vertices==validVertices && larger.indices==validIndices,
            "A cached capture accepted changed nonfinite data or published partial output");
    put(base+positions,0x3F000001);
    require(snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},larger) && larger.vertices==validVertices &&
            larger.indices==validIndices,"A failed capture poisoned a later valid match");
    put(base+descriptor,1365u<<16);
    StoredDraw bounded,boundedAgain;
    require(snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},bounded) &&
            snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},boundedAgain) &&
            bounded.vertices->bytes()==16380 && bounded.vertices==boundedAgain.vertices,
            "The largest position stream below the retention bound was not reusable");
    put(base+descriptor,1366u<<16);
    StoredDraw oversized,oversizedAgain;
    require(snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},oversized) &&
            snapshotImmediateWorldGeometry(base,descriptor,indices,indexCount,{},oversizedAgain) &&
            oversized.vertices->bytes()==16392 && oversized.vertices!=oversizedAgain.vertices,
            "An oversized stream entered the bounded immediate cache");
    std::weak_ptr<const StoredGeometry> uncached=oversized.vertices;
    oversized.vertices.reset();
    require(uncached.expired(),"An oversized snapshot remained owned outside the existing cache");
}
static void immediateIndexContract() {
    auto sentinel=std::make_shared<StoredGeometry>();
    StoredDraw draw{sentinel,sentinel,93,18};
    draw.transforms.emplace();draw.transforms->matrixAddress=999;
    draw.vertexBindings.emplace();draw.vertexBindings->bindingAddress=777;
    auto preserved=[&](const StoredDraw& before) {
        return draw.vertices==before.vertices && draw.indices==before.indices &&
            draw.firstIndex==before.firstIndex && draw.indexCount==before.indexCount &&
            draw.transforms.has_value()==before.transforms.has_value() &&
            (!draw.transforms || draw.transforms->matrixAddress==before.transforms->matrixAddress) &&
            draw.vertexBindings==before.vertexBindings;
    };
    const auto mode=_mm_getcsr();
    for(uint32_t vertices:{0u,1u,2u,32767u,32768u,32769u,65535u,65536u,UINT32_MAX})
    for(uint32_t count:{3u,6u,9u,21u,24u,27u,30u,33u,96u,1023u,49152u}) {
        const uint32_t address=64+count%4;
        std::vector<uint8_t> source(address+count*2);
        std::vector<uint16_t> expected(count);
        auto putIndex=[&](uint32_t index,uint16_t value) {
            source[address+index*2]=uint8_t(value>>8);source[address+index*2+1]=uint8_t(value);
        };
        const uint32_t limit=(std::min)(vertices,65536u);
        for(uint32_t i=0;i<count;++i) {
            expected[i]=uint16_t(vertices?(i*109u+32767u)%limit:65535);
            putIndex(i,expected[i]);
        }
        auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=vertices;
        const auto original=source;
        ImmediateCaptureReason reason;
        if(!vertices) {
            const auto before=draw;
            require(!snapshotImmediateWorldGeometry(source.data(),0,address,count,geometry,draw,&reason) &&
                reason.stage==ImmediateCaptureReason::Stage::indexOob && reason.detail0==0 &&
                reason.detail1==65535 && reason.detail2==0 && preserved(before),
                "Zero-vertex immediate indices changed failure or output");
            require(source==original && _mm_getcsr()==mode,"Failed index conversion changed source or FP state");
            continue;
        }
        require(snapshotImmediateWorldGeometry(source.data(),0,address,count,geometry,draw,&reason) &&
            draw.vertices==geometry && draw.indices->indices==expected && draw.firstIndex==0 &&
            draw.indexCount==count && !draw.transforms && !draw.vertexBindings,
            "Immediate index conversion changed bytes, order or draw metadata");
        const auto owned=draw.indices;
        require(snapshotImmediateWorldGeometry(source.data(),0,address,count,geometry,draw) &&
            (count*2>16384 || draw.indices==owned),"Repeated immediate index retention changed");
        if(vertices>1) {
            expected[0]=uint16_t((expected[0]+1u)%limit);putIndex(0,expected[0]);
            require(snapshotImmediateWorldGeometry(source.data(),0,address,count,geometry,draw) &&
                draw.indices->indices==expected && owned->indices[0]!=expected[0],
                "Changed immediate indices mutated an earlier owned snapshot");
        }
        const auto valid=source;
        if(vertices<=65535) {
            std::set<uint32_t> positions;
            for(uint32_t i=0;i<(std::min)(count,32u);++i)positions.insert(i);
            for(uint32_t i=count/2;i<(std::min)(count,count/2+9);++i)positions.insert(i);
            for(uint32_t i=count>8?count-8:0;i<count;++i)positions.insert(i);
            const auto before=draw;
            for(auto first:positions) {
                source=valid;putIndex(first,uint16_t(vertices));
                // A later error must never replace the first-invalid diagnostic.
                if(first+1<count)putIndex(count-1,65535);
                const auto invalidSource=source;
                require(!snapshotImmediateWorldGeometry(source.data(),0,address,count,geometry,draw,&reason) &&
                    reason.stage==ImmediateCaptureReason::Stage::indexOob && reason.detail0==first &&
                    reason.detail1==vertices && reason.detail2==vertices && preserved(before),
                    "Immediate index failure lost first position, unsigned value or output");
                require(source==invalidSource && _mm_getcsr()==mode,"Index failure changed source or FP state");
            }
            source=valid;
            require(snapshotImmediateWorldGeometry(source.data(),0,address,count,geometry,draw) &&
                draw.indices->indices==expected,"Index rejection poisoned a later valid capture");
        }
        require(source==valid && _mm_getcsr()==mode,"Index capture changed guest bytes or FP state");
    }
    auto* guarded=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    require(guarded!=nullptr,"Allocate guarded immediate-index fixture");
    constexpr uint32_t count=27,address=4096-count*2;
    for(uint32_t i=0;i<count;++i) {guarded[address+i*2]=uint8_t(i);guarded[address+i*2+1]=uint8_t(i*3);}
    DWORD previous=0;
    if(!VirtualProtect(guarded+4096,4096,PAGE_NOACCESS,&previous)) {
        VirtualFree(guarded,0,MEM_RELEASE);require(false,"Protect immediate-index fixture tail");
    }
    auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=65536;
    const bool ended=snapshotImmediateWorldGeometry(guarded,0,address,count,geometry,draw);
    const auto before=draw;ImmediateCaptureReason reason;
    const bool crossed=snapshotImmediateWorldGeometry(guarded,0,address+1,count,geometry,draw,&reason);
    VirtualFree(guarded,0,MEM_RELEASE);
    require(ended && !crossed && reason.stage==ImmediateCaptureReason::Stage::indexCopy &&
        reason.detail0==address+1 && reason.detail1==count && preserved(before) && _mm_getcsr()==mode,
        "Page-ended index capture read beyond its range or published a failed copy");
}
static void finiteScanContract() {
    constexpr std::array<uint32_t,8> finiteBits{0,0x80000000,1,0x80000001,0x007FFFFF,0x807FFFFF,0x7F7FFFFF,0xFF7FFFFF};
    constexpr std::array<uint32_t,6> nonfiniteBits{0x7F800000,0xFF800000,0x7F800001,0xFF800001,0x7FC00000,0xFFC12345};
    for(unsigned count:{1u,2u,3u,7u,129u,32769u})for(unsigned components=1;components<=4;++components)
        for(unsigned gap:{0u,2u,4u}) {
            StoredGeometry g;g.vertexCount=count;g.formats[0]=uint8_t(components);
            g.stride=components*4;
            if(gap) {
                // uint16 and packed byte fields contain all-one bits and must
                // remain exempt. The uint16 gap makes later floats unaligned.
                g.formats[1]=gap==2?9:18;g.formats[11]=uint8_t(5-components);
                g.stride+=gap+(5-components)*4;
            }
            g.vertices.assign(size_t(count)*g.stride,0xFF);
            for(unsigned vertex=0;vertex<count;++vertex) {
                auto* start=g.vertices.data()+size_t(vertex)*g.stride;
                for(unsigned lane=0;lane<components;++lane)put(start+lane*4,finiteBits[(vertex+lane)%finiteBits.size()]);
                if(gap)for(unsigned lane=0;lane<5-components;++lane)
                    put(start+components*4+gap+lane*4,finiteBits[(vertex+lane+3)%finiteBits.size()]);
            }
            const auto original=g.vertices;const auto mode=_mm_getcsr();
            require(validateWorldVertices(g) && g.vertices==original && _mm_getcsr()==mode,
                    "Finite scan rejected an IEEE edge value, inspected packed bytes or changed input/FP state");
            for(unsigned vertex:{0u,(std::min)(1u,count-1),(std::min)(2u,count-1),(std::min)(3u,count-1),count/2,count-1})
                for(unsigned lane=0;lane<(gap?5-components:components);++lane)for(uint32_t bits:nonfiniteBits) {
                const unsigned field=(gap?components*4+gap:0)+lane*4;
                const auto offset=size_t(vertex)*g.stride+field;
                uint32_t previous;std::memcpy(&previous,g.vertices.data()+offset,4);
                put(g.vertices.data()+offset,bits);
                require(!validateWorldVertices(g) && _mm_getcsr()==mode,
                        "Finite scan missed a NaN/infinity in an unaligned field, SIMD boundary or range tail");
                std::memcpy(g.vertices.data()+offset,&previous,4);
            }
            require(g.vertices==original && validateWorldVertices(g),"A failed scan changed source or poisoned a later job");
        }
}
static std::vector<uint8_t> imageFixture(unsigned codec,unsigned width,unsigned height) {
    const unsigned size=((width+3)/4)*((height+3)/4)*(codec?16:8);
    std::vector<uint8_t> bytes(256+16+size);
    put(bytes.data()+64,0x82097610);put(bytes.data()+72,256);put(bytes.data()+76,size+16);
    put(bytes.data()+80,width);put(bytes.data()+84,height);put(bytes.data()+92,4);
    put(bytes.data()+96,0x800);put(bytes.data()+104,0x5014);
    little(bytes.data()+256,codec);little(bytes.data()+260,size);little(bytes.data()+268,16);
    return bytes;
}
static void textureContract() {
    constexpr unsigned width=513,height=257,blocksX=(width+3)/4,blocksY=(height+3)/4;
    for(unsigned codec:{0u,4u}) {
        auto bytes=imageFixture(codec,width,height);const unsigned blockBytes=codec?16:8;
        uint32_t random=0x85664763;
        for(size_t n=272;n<bytes.size();++n){random^=random<<13;random^=random>>17;random^=random<<5;bytes[n]=uint8_t(random);}
        const auto original=bytes;ColorImage image;
        require(!decodeColorImage(bytes.data(),64,image),"Parallel BC decode failed");
        auto block=imageFixture(codec,4,4);
        for(unsigned by=0;by<blocksY;++by)for(unsigned bx=0;bx<blocksX;++bx) {
            std::memcpy(block.data()+272,bytes.data()+272+(by*blocksX+bx)*blockBytes,blockBytes);
            ColorImage expected;require(!decodeColorImage(block.data(),64,expected),"Serial BC oracle failed");
            for(unsigned y=0;y<4 && by*4+y<height;++y)for(unsigned x=0;x<4 && bx*4+x<width;++x)
                require(!std::memcmp(image.pixels.data()+((by*4+y)*width+bx*4+x)*4,expected.pixels.data()+(y*4+x)*4,4),
                    "Parallel BC decode changed a texel or edge crop");
        }
        require(bytes==original,"Parallel BC decode changed the source");
    }
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string_view(argv[1])=="--default-serial") {
            auto& pool=sceneWorkPool();
            require(pool.workers()==0,"Lazy initialization started unrequested workers");
            initializeSceneWorkers();
            const DWORD caller=GetCurrentThreadId();unsigned calls=0;
            require(parallelSceneRange(65535,2048,[&](size_t first,size_t end) {
                ++calls;return GetCurrentThreadId()==caller && first==0 && end==65535;
            }),"Default scene work was split or moved off the caller");
            require(calls==1 && pool.workers()==0 && pool.batches()==0 && pool.assistedChunks()==0,
                "Default serial work used the parallel dispatcher");
            worldDescriptorContract();geometryContract();immediateStreamContract();immediateRetentionContract();immediateIndexContract();finiteScanContract();textureContract();printSceneWorkCounters();
            std::puts("Default scene work passed: no helpers, whole-range serial execution, geometry and texture parity.");
            return 0;
        }
        require(argc==1,"Expected optional --default-serial");
        initializeSceneWorkers(4);
        worldDescriptorContract();executorContract();geometryContract();immediateStreamContract();immediateRetentionContract();immediateIndexContract();finiteScanContract();textureContract();printSceneWorkCounters();
        std::puts("Scene work passed: concurrent execution, nested/concurrent callers, failure recovery, floating-point parity, geometry and BC1/BC3 pixel parity.");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"Scene work: %s\n",e.what());return 1;}
}
