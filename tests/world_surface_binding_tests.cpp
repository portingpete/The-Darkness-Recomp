// CPU-only surface ownership oracle. Calls the original descriptor initializer
// and binder, but never creates a window, graphics device, pixels, or a game loop.
#include "renderer/engine/world_mesh.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_82864840);
extern "C" PPC_FUNC(__imp__sub_82861698);

namespace {
constexpr uint32_t context=0x82A69B00,fixture=0x03000000,extent=65536;
constexpr uint32_t allocation=fixture+0x100,counts=fixture+0x200,binding=fixture+0x300;
constexpr uint32_t rgba=fixture+0x400,a=fixture+0x1000,b=fixture+0x1100;
constexpr uint32_t other=fixture+0x1200,z=fixture+0x1300,texture=fixture+0x3000,device=fixture+0x8000;
constexpr uint32_t color2=fixture+0x1400,color3=fixture+0x1500;
uint64_t originalCalls=0,captures=0;
void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
void put(uint32_t address,uint32_t value) {memory->write32(address,value);}

// Before the correction these commands expose only their raw object pointers.
// This branch lets the same original-code regression demonstrate that failure
// against the preserved runtime, then exercise the production lookup after it.
template<class Command> uint64_t key(const Command& command,unsigned slot) {
    if constexpr(requires {command.surfaceKey(slot);})return command.surfaceKey(slot);
    else return command.targets[slot]?uint64_t(command.targets[slot])*2+(slot==4):0;
}
void initialize(const PPCContext& initial,uint32_t object,uint32_t tile,
                uint32_t width,uint32_t height,unsigned samples,uint32_t format=6,int exponent=0) {
    put(allocation,tile);put(allocation+4,0);put(allocation+8,uint32_t(exponent));
    PPCContext guest;std::memcpy(&guest,&initial,sizeof guest);
    guest.r3.u64=width;guest.r4.u64=height;guest.r5.u64=format;guest.r6.u64=samples;
    guest.r7.u64=allocation;guest.r8.u64=object;guest.r9.u64=counts;guest.r10.u64=counts+4;
    __imp__sub_82864840(guest,memory->base());++originalCalls;
    require(guest.r1.u32==initial.r1.u32,"Original initializer unbalanced stack");
    require((memory->read32(object+28)&0xfff)==tile,"Original initializer changed the storage base");
    require(((memory->read32(object+24)>>16)&3)==samples,"Original initializer changed the sample mode");
}
void bind(const PPCContext& initial,const std::array<uint32_t,5>& targets) {
    put(binding,targets[4]);
    for(unsigned slot=0;slot<4;++slot)put(binding+4+slot*4,targets[slot]);
    PPCContext guest;std::memcpy(&guest,&initial,sizeof guest);
    guest.r3.u64=device;guest.r4.u64=binding;guest.r5.u64=0;
    __imp__sub_82861698(guest,memory->base());++originalCalls;
    require(guest.r1.u32==initial.r1.u32,"Original surface binder unbalanced stack");
    for(unsigned slot=0;slot<5;++slot)
        require(memory->read32(device+12432+slot*4)==targets[slot],"Original binder selected another object");
}
std::array<uint32_t,6> completed() {
    std::array<uint32_t,6> result{};
    for(unsigned i=0;i<6;++i)result[i]=memory->read32(device+10368+i*4);
    return result;
}
StoredDraw setup(uint32_t width,uint32_t height) {
    auto* base=memory->base();std::memset(base+fixture,0,extent);std::memset(base+context,0,18200);
    put(context+15748,device);put(context+17152+8,width);put(context+17152+12,height);
    put(context+17152+20,0x3f800000);
    put(texture+28,2);put(texture+32,0x01000000|6);put(texture+36,(width-1)|((height-1)<<13));
    EngineVertexBindingSnapshot vertex;vertex.deviceAddress=device;vertex.descriptor.flags=0x01000000;
    vertex.descriptor.modes.fill(4);
    const auto encoded=encodeEngineVertexDescriptor(vertex.descriptor);
    for(unsigned i=0;i<5;++i)vertex.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
        uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
    auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=3;geometry->stride=12;
    geometry->formats[0]=3;geometry->vertices.resize(36);geometry->indices={0,1,2};
    StoredDraw draw;draw.vertices=geometry;draw.indices=geometry;draw.indexCount=3;draw.vertexBindings=vertex;
    return draw;
}
struct Commands {WorldDraw draw;WorldClear clear;WorldResolve resolve;};
Commands capture(const StoredDraw& geometry) {
    auto* base=memory->base();
    const std::vector<uint8_t> before(base+fixture,base+fixture+extent),
        contextBefore(base+context,base+context+18200);
    const auto fp=_mm_getcsr();Commands result;
    require(snapshotWorldDraw(base,geometry,result.draw),"Draw metadata fixture rejected");
    require(snapshotWorldClear(base,device,49,rgba,0,0,result.clear),"Clear metadata fixture rejected");
    require(snapshotWorldResolve(base,device,0,0,texture,0,rgba,0,0,0,0,result.resolve),"Resolve metadata fixture rejected");
    require(_mm_getcsr()==fp && !std::memcmp(before.data(),base+fixture,extent) &&
        !std::memcmp(contextBefore.data(),base+context,18200),"Snapshot changed original memory or FP state");
    for(unsigned slot=0;slot<5;++slot) {
        require(result.draw.targets[slot]==memory->read32(device+12432+slot*4),"Snapshot changed bound object");
        require(key(result.draw,slot)==key(result.clear,slot) && key(result.draw,slot)==key(result.resolve,slot),
            "Draw, clear and resolve disagree about storage ownership");
        if(result.draw.targets[slot]) {
            constexpr unsigned infoOffsets[]{10372,10380,10384,10388,10376};
            for(const WorldSurfaceTargets* command:{static_cast<const WorldSurfaceTargets*>(&result.draw),
                static_cast<const WorldSurfaceTargets*>(&result.clear),static_cast<const WorldSurfaceTargets*>(&result.resolve)}) {
                const auto& state=command->surfaceBindings[slot];
                require(state.completed && state.layout==memory->read32(device+10368) &&
                    state.info==memory->read32(device+infoOffsets[slot]),"Owned binding differs from the original completed registers");
            }
        }
    }
    ++captures;return result;
}
void oracle(const PPCContext& initial,uint32_t width,uint32_t height,unsigned samples,uint32_t tile) {
    const auto geometry=setup(width,height);
    initialize(initial,a,tile,width,height,samples);
    const auto narrowTiles=memory->read32(counts);
    initialize(initial,b,tile,width,height,samples);
    initialize(initial,other,tile+32,width,height,samples);
    initialize(initial,z,tile,width,height,samples,23);
    bind(initial,{a,other,0,0,z});const auto first=capture(geometry);const auto original=completed();
    const auto firstKey=key(first.draw,0);
    require(firstKey && key(first.draw,1)!=firstKey && key(first.draw,4)!=firstKey,
        "Distinct storage bases or color/depth planes alias");
    // Tokens represent ownership/command order only. This is not a render or a
    // pixel test: replacing a descriptor must find the existing storage token.
    std::unordered_map<uint64_t,unsigned> contents{{firstKey,71}};
    bind(initial,{b,other,0,0,z});const auto alias=capture(geometry);
    require(completed()==original,"Equivalent original descriptors did not bind identical device state");
    const auto found=contents.find(key(alias.resolve,0));
    if(found==contents.end() || found->second!=71) {
        std::fprintf(stderr,"original storage=%08X/%08X object=%08X->%08X lookup=%016llX->%016llX\n",
            original[0],original[1],a,b,firstKey,key(alias.resolve,0));
        require(false,"Replacing an original surface descriptor lost the current resolve source");
    }
    initialize(initial,a,tile+64,width,height,samples);bind(initial,{a,other,0,0,z});
    const auto recycled=capture(geometry);
    require(!contents.contains(key(recycled.resolve,0)),"Recycled object revived old contents from a different storage base");
    contents[key(recycled.draw,0)]=82;
    bind(initial,{b,other,0,0,z});
    require(contents.at(key(capture(geometry).resolve,0))==71,"Recycled pointer displaced independent retained storage");
    require(key(first.draw,0)==firstKey && key(first.clear,0)==firstKey && key(first.resolve,0)==firstKey,
        "Later binding changed an owned queued command");

    // The GPU consumes the completed device binding. Editing a still-bound
    // descriptor has no effect until the original binder runs again.
    initialize(initial,b,tile+96,width+80,height,samples);
    require(completed()==original,"Descriptor edit unexpectedly changed completed device state");
    require(key(capture(geometry).resolve,0)==firstKey,"Snapshot reread a mutable surface header");
    bind(initial,{b,0,0,0,0});const auto changed=capture(geometry);
    require(key(changed.resolve,0)!=firstKey,"Rebinding changed storage failed to change the lookup");

    // Format/exponent views of the same color allocation must share its stored
    // contents, just as changing those views on one object already did.
    initialize(initial,b,tile,width,height,samples,54);bind(initial,{b,0,0,0,0});
    const auto view=capture(geometry);
    require(key(view.resolve,0)==firstKey,"A color format view allocated unrelated history");
    put(device+12020,1);bind(initial,{b,0,0,0,0});
    require(key(capture(geometry).resolve,0)==firstKey,"Original format adjustment changed storage ownership");
    initialize(initial,b,tile,width,height,samples,63);put(device+12020,0);bind(initial,{b,0,0,0,0});
    const auto floatView=capture(geometry);
    put(device+12020,1);bind(initial,{b,0,0,0,0});
    const auto preciseView=capture(geometry);
    require((floatView.draw.surfaceBindings[0].info&0xf0000)==0x30000 &&
        (preciseView.draw.surfaceBindings[0].info&0xf0000)==0xc0000,
        "Original float view adjustment was not exercised");
    require(key(floatView.resolve,0)==firstKey && key(preciseView.resolve,0)==firstKey,
        "Original 3/12 color views lost shared contents");
    initialize(initial,b,tile,width,height,samples,6,-4);bind(initial,{b,0,0,0,0});
    const auto biased=capture(geometry);
    require(((biased.draw.surfaceBindings[0].info>>20)&63)==60 && key(biased.resolve,0)==firstKey,
        "Color exponent view lost the original allocation");
    for(uint32_t format:{32u,37u}) {
        initialize(initial,b,tile,width,height,samples,format);
        require(memory->read32(counts)==narrowTiles*2,"Original wide allocation did not double tile usage");
        bind(initial,{b,0,0,0,0});
        require(key(capture(geometry).resolve,0)!=firstKey,"A different storage pixel width revived incompatible contents");
    }
    initialize(initial,b,tile,width,height+16,samples);bind(initial,{b,0,0,0,0});
    require(key(capture(geometry).resolve,0)==firstKey,"A taller view of the same pitch lost retained contents");

    initialize(initial,b,tile,width+80,height,samples);bind(initial,{b,0,0,0,0});
    require(key(capture(geometry).resolve,0)!=firstKey,"Different storage pitch reused an incompatible surface");
    initialize(initial,b,tile,width,height,(samples+1)%3);bind(initial,{b,0,0,0,0});
    require(key(capture(geometry).resolve,0)!=firstKey,"Different sample layout reused an incompatible surface");

    initialize(initial,b,tile,width,height,samples);bind(initial,{b,b,b,b,z});
    const auto multiple=capture(geometry);
    for(unsigned slot=0;slot<4;++slot)require(key(multiple.clear,slot)==firstKey,
        "Attachment number replaced physical storage identity");
    initialize(initial,color2,tile+128,width,height,samples);
    initialize(initial,color3,tile+256,width,height,samples);
    bind(initial,{b,other,color2,color3,z});const auto distinct=capture(geometry);
    for(unsigned slot=0;slot<5;++slot)for(unsigned earlier=0;earlier<slot;++earlier)
        require(key(distinct.clear,slot)!=key(distinct.clear,earlier),"Independent attachments aliased one completed register");
    WorldClear synthetic;synthetic.targets=distinct.draw.targets;
    for(unsigned slot=0;slot<5;++slot)require(key(synthetic,slot)!=key(distinct.draw,slot),
        "Synthetic surface identity collided with captured guest storage");
    bind(initial,{0,0,0,0,z});
    auto pooled=captureWorldDraw(memory->base(),geometry);
    require(pooled && key(*pooled,4)==key(first.draw,4),"Depth-only binding lost retained depth storage");
    pooled.reset();bind(initial,{0,0,0,0,0});const auto empty=capture(geometry);
    pooled=captureWorldDraw(memory->base(),geometry);
    require(bool(pooled),"Empty pooled capture failed");
    for(unsigned slot=0;slot<5;++slot)require(!key(empty.resolve,slot) && !key(*pooled,slot),
        "Null binding or pooled draw revived stale completed words");
    auto unchanged=first.clear;
    require(!snapshotWorldClear(memory->base(),0xfffffff0,49,rgba,0,0,unchanged) &&
        key(unchanged,0)==firstKey,"Failed capture published pointer-only or partial storage metadata");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Expected game directory");
        Memory owner;memory=&owner;owner.load(argv[1]);PPCContext initial{};owner.initThread(initial);
        require(owner.commit(fixture,extent),"Cannot commit CPU fixture");
        for(unsigned samples:{0u,1u,2u})for(uint32_t tile:{0u,1024u})oracle(initial,64,32,samples,tile);
        oracle(initial,1720,720,0,0);
        std::printf("WorldSurfaceBindingContract passed: %llu captures, %llu original-AOT calls; descriptor replacement/reuse, completed bindings, clear/draw/resolve ownership, pitch/sample/pixel widths, format/exponent views, all attachments, synthetic isolation, null/invalid bindings and queued/pool lifetime. CPU only.\n",captures,originalCalls);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"WorldSurfaceBindingContract failed: %s\n",e.what());return 1;
    }
}
