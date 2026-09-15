#include "renderer/engine/stored_geometry.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/render_trace.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_82762328);
extern "C" PPC_FUNC(__imp__sub_82899BF0);
extern "C" PPC_FUNC(__imp__sub_82765410);
extern "C" PPC_FUNC(__imp__sub_82760598);
static void require(bool result, const char* why) { if (!result) throw std::runtime_error(why); }
static void put16(uint8_t* b, uint32_t a, uint16_t v) { b[a] = uint8_t(v >> 8); b[a+1] = uint8_t(v); }
static void put32(uint8_t* b, uint32_t a, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[a+i] = uint8_t(v >> ((3-i)*8));
}
static void putFloat(uint8_t* b, uint32_t a, float v) { put32(b, a, std::bit_cast<uint32_t>(v)); }
constexpr uint32_t fixture = 0x03000000, extent = 131072, context = 0x82A69B00;
constexpr uint32_t manager = fixture+0x3000, validity = fixture+0x3200, table = fixture+0x3400;
constexpr uint32_t descriptor = fixture+0x5000, formats = fixture+0x5200, constants = fixture+0x5300;
constexpr uint32_t positions = fixture+0x5400, uv = fixture+0x5500, destination = fixture+0x5600;
constexpr uint32_t indexOutput = fixture+0x5800, indexSource = fixture+0x5900;
constexpr uint32_t cursor = fixture+0x6000, countAddress = fixture+0x6040, packet = fixture+0x6080;
constexpr uint32_t indexBuffer = fixture+0x5F80;
static uint32_t resource(unsigned id) { return fixture + id * 128; }

static void initialize(uint8_t* base, unsigned id, uint32_t stride = 20, unsigned triangles = 2) {
    const auto address = resource(id);
    std::memset(base+address, 0, 108);
    put16(base,address+92,uint16_t(id)); put16(base,address+94,uint16_t(triangles)); base[address+96]=uint8_t(stride);
    put32(base,address+8,fixture+0x5F00); // Opaque identities, not a hardware fixture.
    put32(base,address+48,fixture+0x5F40); put32(base,address+88,fixture+0x5F80);
    put32(base,table+(id+1)*4,address); base[validity+id]=1;
}
static void streams(uint8_t* base) {
    std::memset(base+descriptor,0,448); std::memset(base+formats,0,16); std::memset(base+constants,0,160);
    put32(base,descriptor,positions); put32(base,descriptor+4,uv); put32(base,descriptor+424,4);
    base[descriptor+392]=base[formats]=3; base[descriptor+393]=base[formats+1]=2;
    for(unsigned i=0;i<4;++i) {
        putFloat(base,positions+i*12,float(i)/4); putFloat(base,positions+i*12+4,float(i)/8);
        putFloat(base,positions+i*12+8,0.5f); putFloat(base,uv+i*8,float(i)/4); putFloat(base,uv+i*8+4,0.75f);
        for(unsigned j=0;j<20;++j) base[destination+i*20+j]=uint8_t(i*20+j);
    }
    const uint16_t indices[]{0,1,2,1,3,2};
    for(unsigned i=0;i<6;++i) put16(base,indexOutput+i*2,indices[i]);
}
// Compare the whole register ABI, fixture and touched stack, not just r3.
static void compareOriginal(PPCFunc* original, PPCFunc* wrapped, PPCContext initial, uint8_t* base) {
    std::vector<uint8_t> before(base+fixture,base+fixture+extent);
    const uint32_t stack = initial.r1.u32-4096;
    std::vector<uint8_t> stackBefore(base+stack,base+stack+4224);
    PPCContext expected; std::memcpy(&expected,&initial,sizeof(expected));
    original(expected,base);
    std::vector<uint8_t> after(base+fixture,base+fixture+extent), stackAfter(base+stack,base+stack+4224);
    std::memcpy(base+fixture,before.data(),before.size()); std::memcpy(base+stack,stackBefore.data(),stackBefore.size());
    PPCContext actual; std::memcpy(&actual,&initial,sizeof(actual));
    wrapped(actual,base);
    require(!std::memcmp(&expected,&actual,sizeof(actual)),"Stored observer changed original register ABI");
    require(!std::memcmp(base+fixture,after.data(),after.size()),"Stored observer changed original fixture output");
    require(!std::memcmp(base+stack,stackAfter.data(),stackAfter.size()),"Stored observer changed original stack output");
}

static void originalContracts(uint8_t* base, PPCContext initial) {
    streams(base);
    for (bool quantized : {false,true}) {
        base[formats]=quantized?15:3;
        for(unsigned i=0;i<4;++i) putFloat(base,constants+i*4,1.0f);
        initial.r3.u32=descriptor; initial.r4.u32=destination; initial.r5.u32=formats;
        initial.r6.u32=constants; initial.r7.u32=quantized?1:0;
        initial.r8.u32=0xDEADBEEF; // Deliberately not a count: original reads descriptor+424.
        initial.r22.u32=resource(40); initial.lr=0x82252C84;
        compareOriginal(__imp__sub_82762328,sub_82762328,initial,base);
        const unsigned stride=quantized?12:20;
        initialize(base,40,stride);
        StoredGeometryCache cache;
        auto upload=cache.begin(base,resource(40));
        cache.vertices(upload,base,descriptor,destination,formats,constants,quantized?1:0);
        cache.indices(upload,base,indexOutput,6,6);
        const std::vector<uint8_t> originalBytes(base+destination,base+destination+stride*4);
        require(cache.finish(std::move(upload),base),"Original converted upload could not be published");
        const auto owned=cache.draw(base,40,40,0,6,indexBuffer);
        require(owned && owned.vertices->vertices==originalBytes && owned.vertices->formats[0]==(quantized?15:3) &&
                owned.vertices->conversionMask==(quantized?1u:0u),"Original conversion bytes or metadata changed");
    }
    for(unsigned i=0;i<6;++i) put16(base,indexSource+i*2,uint16_t(5-i));
    for(uint32_t caller : {0x82252DD4u,0x12345678u}) {
        initial.r3.u32=indexOutput; initial.r4.u32=indexSource; initial.r5.u32=12; initial.lr=caller;
        compareOriginal(__imp__sub_82899BF0,sub_82899BF0,initial,base);
    }
    for(unsigned type : {1u,3u,4u}) {
        std::memset(base+cursor,0,512);
        const unsigned words=type==1?5:6, count=type==1?3:6;
        put32(base,cursor,packet); put32(base,cursor+12,packet+words*2); put32(base,countAddress,count);
        put16(base,packet,uint16_t((words<<8)|type)); put16(base,packet+2,type==1?1:4);
        for(unsigned i=0;i<(type==1?3u:4u);++i) put16(base,packet+4+i*2,uint16_t(i));
        initial.r3.u32=cursor; initial.r4.u32=indexOutput; initial.r5.u32=countAddress; initial.lr=0x8225342C;
        compareOriginal(__imp__sub_82765410,sub_82765410,initial,base);
        require(memory->read32(countAddress)==count,"Original stored expansion produced unexpected count");
        StoredGeometryCache cache;
        streams(base); // Preserve the original produced indices separately below.
        // The ABI comparison above covers the actual list/strip/fan writer.
        auto upload=cache.begin(base,resource(40));
        cache.indices(upload,base,indexOutput,count,count-1);
        require(upload.failed,"Partial stored expansion was treated as complete");
    }
    require(storedGeometryCache().stats().published==0,"Unscoped helper call published a stored resource");
}

static void cacheContracts(uint8_t* base) {
    streams(base); initialize(base,40);
    StoredGeometryCache cache(1024,2);
    auto prepare=[&](StoredGeometryCache& target,unsigned id) {
        initialize(base,id); auto upload=target.begin(base,resource(id));
        target.vertices(upload,base,descriptor,destination,formats,constants,0);
        target.indices(upload,base,indexOutput,6,6); return upload;
    };
    auto upload=prepare(cache,40);
    require(cache.finish(std::move(upload),base),"Valid stored resource rejected");
    auto retained=cache.draw(base,40,40,3,3,indexBuffer);
    require(cache.vertexStream(base,40,fixture+0x5F40)==retained.vertices &&
            !cache.vertexStream(base,40,0) && !cache.vertexStream(base,40,fixture+0x5F44),
            "Immediate indices accepted a different bound vertex buffer");
    require(retained && retained.firstIndex==3 && retained.indices->indices[3]==1,"Stored offset is not an index offset");
    const auto originalBytes=retained.vertices->vertices;
    std::memset(base+destination,0xFF,80); put16(base,indexOutput+6,100);
    require(retained.vertices->vertices==originalBytes && retained.indices->indices[3]==1,"Owned upload retained guest pointers");
    require(!cache.draw(base,40,40,6,3,indexBuffer) && !cache.draw(base,40,40,0xFFFFFFFE,3,indexBuffer) && !cache.draw(base,40,40,0,2,indexBuffer),
            "Out-of-bounds, overflowing or incomplete draw range accepted");
    base[validity+40]=0; require(!cache.draw(base,40,40,0,3,indexBuffer),"Invalidated engine resource remained drawable"); base[validity+40]=1;
    put32(base,table+41*4,resource(41)); require(!cache.draw(base,40,40,0,3,indexBuffer),"Replaced table entry reused cached data");
    put32(base,table+41*4,resource(40));
    base[resource(40)+96]=24; require(!cache.draw(base,40,40,0,3,indexBuffer),"Changed declaration stride reused cached data"); base[resource(40)+96]=20;
    put32(base,resource(40)+100,1); require(!cache.draw(base,40,40,0,3,indexBuffer),"Changed declaration flags reused cached data");
    put32(base,resource(40)+100,0);
    put16(base,resource(40)+94,1);
    require(!cache.draw(base,40,40,0,3,indexBuffer),"Shared-resource validation ignored the changed index descriptor");
    put16(base,resource(40)+94,2);
    require(!cache.draw(base,40,40,0,3,0) && !cache.draw(base,40,40,0,3,indexBuffer+16),"Wrong actual bound IB accepted");
    put32(base,resource(40)+88,fixture+0x5FC0); require(!cache.draw(base,40,40,0,3,indexBuffer),"Rebound IB reused cached data");
    put32(base,resource(40)+88,fixture+0x5F80);

    streams(base);
    auto older=prepare(cache,40), newer=prepare(cache,40);
    require(cache.finish(std::move(newer),base) && !cache.finish(std::move(older),base) && cache.draw(base,40,40,0,3,indexBuffer),
            "Out-of-order completion resurrected old generation or removed new upload");
    auto failed=cache.begin(base,resource(40));
    cache.vertices(failed,base,descriptor,0xFFFFFFF8,formats,constants,0);
    require(!cache.finish(std::move(failed),base) && !cache.draw(base,40,40,0,3,indexBuffer),"Failed replacement retained stale resource");
    require(retained.vertices->vertices==originalBytes,"Invalidation destroyed retained immutable draw");

    require(cache.finish(prepare(cache,40),base) && cache.finish(prepare(cache,41),base) && cache.finish(prepare(cache,42),base),
            "Valid cache replacements failed");
    require(cache.stats().cachedResources==2 && cache.stats().cachedBytes==184 && !cache.draw(base,40,40,0,3,indexBuffer),
            "Stored cache entry limit or LRU eviction failed");
    StoredGeometryCache bytesLimited(100,256);
    require(bytesLimited.finish(prepare(bytesLimited,40),base) && bytesLimited.finish(prepare(bytesLimited,41),base) &&
            bytesLimited.stats().cachedResources==1 && bytesLimited.stats().cachedBytes==92,"Stored byte budget failed");

    // Distinct IB/VB IDs can use a smaller valid subset of a larger IB.
    put32(base,descriptor+424,3); initialize(base,43);
    auto small=cache.begin(base,resource(43)); cache.vertices(small,base,descriptor,destination,formats,constants,0);
    require(cache.finish(std::move(small),base),"Vertex-only upload rejected");
    require(cache.draw(base,43,42,0,3,indexBuffer) && !cache.draw(base,43,42,3,3,indexBuffer),"Index range was not checked against selected VB");
    put32(base,descriptor+424,4);
    base[validity+42]=0;
    require(!cache.draw(base,43,42,0,3,indexBuffer),"Separate index resource skipped liveness validation");
    base[validity+42]=1;
    base[validity+43]=0;
    require(!cache.draw(base,43,42,0,3,indexBuffer),"Separate vertex resource skipped liveness validation");
    base[validity+43]=1;
    auto bad=prepare(cache,44); bad.failed=false; // Exercise a fresh invalid format without an earlier failure.
    bad.geometry->vertices.clear(); base[formats]=27;
    cache.vertices(bad,base,descriptor,destination,formats,constants,0);
    require(!cache.finish(std::move(bad),base),"Unknown converted format was accepted"); base[formats]=3;
    auto tooMany=cache.begin(base,resource(44)); put32(base,descriptor+424,65536);
    cache.vertices(tooMany,base,descriptor,destination,formats,constants,0);
    require(tooMany.failed,"Unbounded vertex allocation was accepted"); put32(base,descriptor+424,4);

    for(unsigned kind=0;kind<4;++kind) {
        streams(base); initialize(base,44);
        if(kind==0) base[descriptor+393]=0; // Destination has a slot the converter skips.
        if(kind==1) base[formats+1]=0; // Original would assert for the present source slot.
        if(kind==2) base[descriptor+393]=27;
        if(kind==3) put32(base,descriptor+4,0);
        auto mismatched=cache.begin(base,resource(44));
        cache.vertices(mismatched,base,descriptor,destination,formats,constants,0);
        require(mismatched.failed && !cache.finish(std::move(mismatched),base),"Invalid source/output layout accepted");
    }
    streams(base); initialize(base,44);
    auto unbound=cache.begin(base,resource(44));
    cache.vertices(unbound,base,descriptor,destination,formats,constants,0);
    put32(base,resource(44)+48,0);
    require(!cache.finish(std::move(unbound),base),"Resource without a completed VB binding was published");
}

static void drawRouteContracts(uint8_t* base) {
    // Original executable oracle: whole-resource count is BE16 resource+94,
    // first index/base vertex are zero, primitive is triangle list, and the
    // indexed call returns to 8225E2E0. No guessed r5/r7 entry arguments.
    require(memory->read32(0x8225E2C0)==0xA17D005E && memory->read32(0x8225E2C4)==0x38C00000 &&
            memory->read32(0x8225E2CC)==0x38A00000 && memory->read32(0x8225E2D0)==0x38800004 &&
            memory->read32(0x8225E2DC)==0x4860AD0D,"Original whole-resource draw ABI changed");
    constexpr uint32_t vertexBytes=fixture+0x10000, indexBytes=fixture+0x19000;
    constexpr uint32_t pistolVertices=1716, pistolIndices=4344;
    streams(base); initialize(base,20,20,pistolIndices/3);
    put32(base,descriptor+424,pistolVertices);
    for (unsigned i=0;i<pistolVertices*20;++i) base[vertexBytes+i]=uint8_t(i*37);
    for (unsigned i=0;i<pistolIndices;++i) put16(base,indexBytes+i*2,uint16_t(i%pistolVertices));
    StoredGeometryCache cache;
    auto upload=cache.begin(base,resource(20));
    cache.vertices(upload,base,descriptor,vertexBytes,formats,constants,0);
    cache.indices(upload,base,indexBytes,pistolIndices,pistolIndices);
    require(cache.finish(std::move(upload),base),"Pistol-sized stored upload rejected");
    streams(base);
    for (unsigned id : {40u,41u}) {
        initialize(base,id); auto subsetUpload=cache.begin(base,resource(id));
        cache.vertices(subsetUpload,base,descriptor,destination,formats,constants,0);
        cache.indices(subsetUpload,base,indexOutput,6,6);
        require(cache.finish(std::move(subsetUpload),base),"Subset replay upload rejected");
    }
    const auto whole=StoredDrawRequest::whole(20);
    const StoredDrawRequest subset{40,41,1,3};
    struct Boundary { uint32_t caller, first, count, vertexId; };
    // Correct elevator inspection: both pistols in depth, motion, LF and
    // NDSEATP passes. Interleave existing subset draws to detect lost order
    // or deduplication by resource; the original immediate caller is separate.
    const Boundary sequence[]{
        {0x8225E2E0,0,4344,20},{0x8225E2E0,0,4344,20},{0x8225E3B8,3,3,40},
        {0x8225E2E0,0,4344,20},{0x8225E2E0,0,4344,20},{0x8225E3B8,3,3,40},
        {0x8225E2E0,0,4344,20},{0x8225E2E0,0,4344,20},
        {0x8225E2E0,0,4344,20},{0x8225E2E0,0,4344,20}};
    std::vector<StoredDraw> ordered;
    for (const auto& call : sequence) {
        for (const auto* request : {&whole,&subset}) {
            auto draw=request->resolve(cache,base,call.caller,4,0,call.first,call.count,call.vertexId,indexBuffer);
            if (draw) ordered.push_back(std::move(draw));
        }
    }
    require(ordered.size()==std::size(sequence) && cache.stats().matchedDraws==std::size(sequence),
            "Whole/subset routes omitted or duplicated a draw");
    for (size_t i=0;i<ordered.size();++i) {
        const auto& draw=ordered[i]; const auto& call=sequence[i];
        require(draw.vertices->id==call.vertexId && draw.indices->id==(call.vertexId==20?20u:41u) &&
                draw.firstIndex==call.first && draw.indexCount==call.count,"Stored draw order or selected buffers changed");
    }
    require(ordered[0].vertices->vertexCount==pistolVertices && ordered[0].indices->indices.back()==911 &&
            ordered[0].vertices==ordered[1].vertices,"Whole draw lost complete owned geometry");
    for (uint32_t caller : {0x8225DD38u,0x8225E3B8u,0x12345678u})
        require(!whole.resolve(cache,base,caller,4,0,0,4344,20,indexBuffer),"Whole request consumed another draw route");
    require(!subset.resolve(cache,base,0x8225E2E0,4,0,0,4344,20,indexBuffer) &&
            !subset.resolve(cache,base,0x8225DD38,4,0,3,3,40,indexBuffer),"Existing route duplicated a whole/immediate draw");
    require(!whole.resolve(cache,base,0x8225E2E0,4,0,0,4344,40,indexBuffer) &&
            !whole.resolve(cache,base,0x8225E2E0,4,0,0,4344,20,indexBuffer+16) &&
            !whole.resolve(cache,base,0x8225E2E0,4,0,0,4341,20,indexBuffer) &&
            !whole.resolve(cache,base,0x8225E2E0,4,0,3,4341,20,indexBuffer) &&
            !whole.resolve(cache,base,0x8225E2E0,4,1,0,4344,20,indexBuffer) &&
            !whole.resolve(cache,base,0x8225E2E0,5,0,0,4344,20,indexBuffer),
            "Whole route accepted a mismatched binding, partial resource or different primitive");
    base[validity+20]=0;
    require(!whole.resolve(cache,base,0x8225E2E0,4,0,0,4344,20,indexBuffer),"Whole route resurrected invalid geometry");
    base[validity+20]=1;
    std::memset(base+vertexBytes,0,pistolVertices*20); std::memset(base+indexBytes,0,pistolIndices*2);
    require(ordered[0].vertices->vertices[1]==37 && ordered[0].indices->indices.back()==911,
            "Whole route retained guest buffer pointers");
}

static void attributeContracts(uint8_t* base, PPCContext initial) {
    constexpr uint32_t input = fixture+0x7000, output = fixture+0x8000, samples = 64;
    uint32_t random = 0x51A7C0DE;
    uint64_t comparisons = 0;
    // This table independently comes from the original loaded image.
    const auto sizes = base+0x82A3D31C;
    require(memory->read32(0x8209DF0C)==0x3A001002 && memory->read32(0x8209DF08)==0x3A802008 &&
            memory->read32(0x8209DFE0)==0x3B004020 && memory->read32(0x8209DCCC)==0x3B808081,
            "Packed attribute constants differ from original executable");
    for (uint8_t format : {1,2,3,4,9,10,11,12,14,15,16,17,18})
    for (unsigned slot : {0u,1u,9u,12u})
    for (bool scaled : {false,true}) {
        StoredGeometry geometry;
        if (slot) geometry.formats[0]=1; // Verify nonzero semantic offsets.
        geometry.formats[slot]=format;
        const unsigned offset=slot?4:0;
        geometry.stride=offset+sizes[format]; geometry.vertexCount=samples;
        geometry.conversionMask=scaled?(1u<<(slot<5?slot:0)):0;
        std::memset(base+constants,0,160);
        const float scales[]{39.897132873535156f,16.449974060058594f,24.985279083251953f,1.0f};
        const float offsets[]{-0.636924147605896f,-8.224987030029297f,-12.292546272277832f,0.0f};
        for(unsigned i=0;i<4;++i) {
            putFloat(base,constants+(slot<5?slot:0)*32+i*4,scales[i]);
            putFloat(base,constants+(slot<5?slot:0)*32+16+i*4,offsets[i]);
        }
        std::memcpy(geometry.conversionConstants.data(),base+constants,160);
        for(unsigned vertex=0;vertex<samples;++vertex) {
            const auto address=input+vertex*geometry.stride+offset;
            for(unsigned i=0;i<sizes[format];++i) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                base[address+i]=vertex==0?0:(vertex==1?255:uint8_t(random));
            }
            if(format<=4) for(unsigned i=0;i<format;++i)
                putFloat(base,address+i*4,float(int32_t((random>>(i*4))&65535)-32768)/1024.0f);
        }
        geometry.vertices.assign(base+input,base+input+samples*geometry.stride);
        PPCContext oracle; std::memcpy(&oracle,&initial,sizeof(oracle));
        oracle.r3.u32=input+offset; oracle.r4.u32=format; oracle.r5.u32=geometry.stride;
        oracle.r6.u32=scaled && slot<5?constants+slot*32:0;
        oracle.r7.u32=output; oracle.r8.u32=4; oracle.r9.u32=16; oracle.r10.u32=0;
        put32(base,oracle.r1.u32+84,samples);
        __imp__sub_82760598(oracle,base);
        for(unsigned vertex=0;vertex<samples;++vertex) {
            std::array<float,4> decoded;
            require(decodeStoredAttribute(geometry,vertex,slot,decoded),"Supported original attribute rejected");
            for(unsigned i=0;i<4;++i) {
                const uint32_t expected=memory->read32(output+vertex*16+i*4);
                if(std::bit_cast<uint32_t>(decoded[i])!=expected) {
                    std::fprintf(stderr,"Attribute mismatch: format=%u slot=%u scaled=%u vertex=%u component=%u host=%08X original=%08X\n",
                        format,slot,unsigned(scaled),vertex,i,std::bit_cast<uint32_t>(decoded[i]),expected);
                    require(false,"Host attribute decode differs from original CPU converter");
                }
                ++comparisons;
            }
        }
    }
    StoredGeometry invalid;
    invalid.vertexCount=1; invalid.stride=12; invalid.formats[0]=3; invalid.vertices.resize(12);
    const std::array<float,4> sentinel{91,92,93,94};
    auto value=sentinel;
    require(!decodeStoredAttribute(invalid,1,0,value) && !decodeStoredAttribute(invalid,0,16,value) &&
            !decodeStoredAttribute(invalid,0,9,value) && value==sentinel,"Invalid attribute range changed output");
    invalid.vertices.pop_back();
    require(!decodeStoredAttribute(invalid,0,0,value) && value==sentinel,"Truncated owned vertex was decoded");
    invalid.vertices.resize(12); invalid.formats[0]=27;
    require(!decodeStoredAttribute(invalid,0,0,value),"Unknown format was decoded");
    invalid.formats[0]=13; invalid.stride=6; invalid.vertices.resize(6);
    require(!decodeStoredAttribute(invalid,0,0,value),"Unimplemented format was decoded");
    invalid.formats[0]=3; invalid.stride=12; invalid.vertices.resize(12);
    invalid.vertices[0]=0x7F; invalid.vertices[1]=0x80;
    require(!decodeStoredAttribute(invalid,0,0,value) && value==sentinel,"Nonfinite vertex changed output");
    invalid.vertices[0]=0; invalid.vertices[1]=0; invalid.conversionMask=1;
    invalid.conversionConstants[0]=0x7F; invalid.conversionConstants[1]=0x80;
    require(!decodeStoredAttribute(invalid,0,0,value) && value==sentinel,"Nonfinite conversion constant changed output");
    std::printf("Stored attributes: %llu bit-exact float comparisons against original 82760598 across 13 formats, 4 slots, scaling and boundary/random values.\n",comparisons);
}

static void indexedCacheContracts(uint8_t* base) {
    const auto area=memory->allocate(0x100000);
    require(area!=0,"Allocate resource index fixture");
    const auto oldTable=memory->read32(context+16636),oldManager=memory->read32(context+160);
    const auto largeTable=area,flags=area+0x41000,owner=area+0x52000,resources=area+0x60000;
    std::memset(base+area,0,0x100000);
    put32(base,context+16636,largeTable);put32(base,context+160,owner);put32(base,owner+24,flags);
    auto prepare=[&](StoredGeometryCache& cache,unsigned id,unsigned slot) {
        const auto address=resources+slot*128;
        std::memset(base+address,0,108);
        put16(base,address+92,uint16_t(id));put16(base,address+94,1);base[address+96]=12;
        put32(base,address+12,1);put32(base,address+52,1);
        put32(base,largeTable+(id+1)*4,address);base[flags+id]=1;
        auto upload=cache.begin(base,address);
        require(bool(upload.geometry),"Begin indexed resource");
        auto& geometry=*upload.geometry;
        geometry.vertexCount=3;geometry.stride=12;geometry.formats[0]=3;
        geometry.vertices.resize(36);geometry.vertices[0]=uint8_t(slot);
        geometry.indices={0,1,2};geometry.maximumIndex=2;
        return upload;
    };
    auto publish=[&](StoredGeometryCache& cache,unsigned id,unsigned slot) {
        require(cache.finish(prepare(cache,id,slot),base),"Publish indexed resource");
    };
    auto draw=[&](StoredGeometryCache& cache,unsigned id,unsigned slot) {
        return cache.draw(base,id,id,0,3,resources+slot*128+56);
    };
    StoredGeometryCache cache(1024*1024,512);
    publish(cache,1,1);
    const auto retained=draw(cache,1,1);
    require(bool(retained),"Initial indexed draw failed");
    for(unsigned id=2;id<=384;++id)publish(cache,id,id);
    publish(cache,65535,385);
    for(unsigned id=1;id<=384;++id) {
        const auto result=draw(cache,id,id);
        require(bool(result) && result.vertices->id==id && result.vertices->vertices[0]==uint8_t(id),
                "Map growth changed indexed draw identity");
        require(cache.vertexStream(base,id,resources+id*128+16)==result.vertices,
                "Vertex stream index differs after map growth");
    }
    require(draw(cache,65535,385).vertices->id==65535,"Largest original ID was not indexed");
    for(unsigned id:{0u,65536u,0xFFFFFFFFu})
        require(!draw(cache,id,1) && !cache.vertexStream(base,id,resources+144),"Out-of-range index accepted");
    // Replacement at a different address must retire the old lookup while
    // already queued draws continue owning their original bytes.
    publish(cache,1,386);
    require(draw(cache,1,386).vertices!=retained.vertices && retained.vertices->vertices[0]==1,
            "Replacement changed an already retained draw");
    auto failed=prepare(cache,1,386);failed.failed=true;
    require(!cache.finish(std::move(failed),base) && !draw(cache,1,386),"Failed replacement retained an indexed entry");
    // Reuse one guest address with another ID, then restore the old header.
    // A stale by-ID pointer must remain absent even when live checks match again.
    const auto old=draw(cache,2,2);
    publish(cache,500,2);
    put16(base,resources+2*128+92,2);
    require(!draw(cache,2,2) && !cache.vertexStream(base,2,resources+2*128+16),
            "Address reuse left a stale resource index");
    require(old.vertices->id==2 && old.vertices->vertices[0]==2,"Address reuse changed owned geometry");
    put16(base,resources+2*128+92,500);
    require(bool(draw(cache,500,2)),"New identity missing after address reuse");
    auto invalid=prepare(cache,1,386);invalid.geometry->id=65536;
    const auto bytes=cache.stats().cachedBytes;
    require(!cache.finish(std::move(invalid),base) && cache.stats().cachedBytes==bytes,
            "Malformed upload ID changed cache accounting");
    for(bool byteBudget:{false,true}) {
        StoredGeometryCache tiny(byteBudget?42:1024,byteBudget?512:1);
        publish(tiny,600,400);
        const auto held=draw(tiny,600,400);
        publish(tiny,601,401);
        require(!draw(tiny,600,400) && !tiny.vertexStream(base,600,resources+400*128+16) &&
                bool(draw(tiny,601,401)) && held.vertices->id==600,
                "Eviction left a stale index or invalidated retained geometry");
        publish(tiny,600,400);
        require(bool(draw(tiny,600,400)) && !draw(tiny,601,401) && tiny.stats().cachedBytes==42 &&
                tiny.stats().cachedResources==1 && tiny.stats().evicted==2,"Evicted ID could not be safely reused");
    }
    put32(base,context+16636,oldTable);put32(base,context+160,oldManager);
    require(memory->release(area),"Release resource index fixture");
    std::puts("Stored index: map growth, ID bounds, replacement, failed upload, address reuse, byte/entry eviction and retained draw lifetimes passed.");
}

static void threadedContracts(uint8_t* base) {
    // Lock-narrowing proof: concurrent draw()/vertexStream() readers on stable
    // entries plus a writer replacing DISJOINT ids. Readers must match every
    // time (no spurious generation miss); the writer's replacements must never
    // leak into another id's draw. Shared descriptor regions stay read-only
    // during the concurrent phase; per-id table/validity/resource bytes are
    // disjoint across threads.
    streams(base);
    StoredGeometryCache cache;
    auto publish = [&](unsigned id) {
        initialize(base, id);
        auto upload = cache.begin(base, resource(id));
        cache.vertices(upload, base, descriptor, destination, formats, constants, 0);
        cache.indices(upload, base, indexOutput, 6, 6);
        require(cache.finish(std::move(upload), base), "Threaded fixture upload rejected");
    };
    publish(50); publish(51); publish(60); publish(61); publish(62); publish(63);
    constexpr int readers = 6, draws = 2000, writes = 300;
    std::atomic<int> matched{0};
    std::atomic<bool> failed{false};
    auto read = [&]() {
        try {
            for (int i = 0; i < draws; ++i) {
                auto same = cache.draw(base, 50, 50, 0, 6, indexBuffer);
                auto split = cache.draw(base, 50, 51, 0, 6, indexBuffer);
                auto stream = cache.vertexStream(base, 50, fixture + 0x5F40);
                if (!same || !split || !stream || stream.get() != same.vertices.get()) { failed = true; return; }
                matched += 3;
            }
        } catch (...) { failed = true; }
    };
    auto write = [&]() {
        try {
            for (int i = 0; i < writes; ++i) {
                const unsigned id = 60 + (i % 4);
                initialize(base, id);
                auto upload = cache.begin(base, resource(id));
                cache.vertices(upload, base, descriptor, destination, formats, constants, 0);
                cache.indices(upload, base, indexOutput, 6, 6);
                if (!cache.finish(std::move(upload), base)) { failed = true; return; }
            }
        } catch (...) { failed = true; }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < readers; ++i) threads.emplace_back(read);
    threads.emplace_back(write);
    for (auto& t : threads) t.join();
    require(!failed && matched == readers * draws * 3, "Concurrent draw/stream missed or mismatched stable entries");
    require(cache.draw(base, 50, 50, 0, 6, indexBuffer) && cache.draw(base, 50, 51, 0, 6, indexBuffer),
            "Stable entries lost after concurrent replacement of disjoint ids");
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Expected original game directory");
        configureRenderTrace({}); enableEnginePreview();
        Memory owner; memory=&owner; owner.load(argv[1]); PPCContext initial{}; owner.initThread(initial);
        require(owner.commit(fixture,extent),"Cannot commit stored geometry fixture"); auto* base=owner.base();
        std::memset(base+fixture,0,extent);
        put32(base,context+160,manager); put32(base,manager+24,validity); put32(base,context+16636,table);
        originalContracts(base,initial); cacheContracts(base); drawRouteContracts(base); attributeContracts(base,initial);
        indexedCacheContracts(base); threadedContracts(base);
        std::puts("StoredGeometryContract passed: original converter/copy/packed ABI, original attribute values, owned bytes, generation invalidation, actual IB binding, separate IB/VB ranges, cache budgets and concurrent draw/stream stability.");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"StoredGeometryContract failed: %s\n",e.what()); return 1; }
}
