// CPU-only original-AOT binding oracle. No window, graphics device, rendering,
// image decoding, or pixel inspection is performed by this executable.
#include "renderer/engine/world_mesh.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_822569E0);
extern "C" PPC_FUNC(__imp__sub_82864F20);

namespace {
constexpr uint32_t context=0x82A69B00,fixture=0x03000000,extent=65536;
constexpr uint32_t ids=fixture+0x100,table=fixture+0x200,metadata=fixture+0x300;
constexpr uint32_t validBits=fixture+0x400,resource=fixture+0x800;
constexpr uint32_t primary=fixture+0x1000,alternate=fixture+0x1100,direct=fixture+0x1200;
constexpr uint32_t program=fixture+0x2000,name=fixture+0x2100,device=fixture+0x4000;
uint64_t comparisons=0,originalCalls=0;
void require(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
void put(uint32_t address,uint32_t value) { memory->write32(address,value); }
void half(uint32_t address,uint16_t value) {
    auto* base=memory->base();base[address]=uint8_t(value>>8);base[address+1]=uint8_t(value);
}
void header(uint32_t object,uint32_t storage) {
    put(object+28,2);
    put(object+32,storage|6);
    put(object+36,31u|(31u<<13));
    put(object+40,0xd10);
    put(object+44,5u<<6);
}
StoredDraw setup(unsigned slot,bool alternateSelected,bool inlineHeader,uint32_t alias) {
    auto* base=memory->base();
    std::memset(base+fixture,0,extent);std::memset(base+context,0,18200);
    put(context+15748,device);put(context+17964,table);put(context+144,metadata);
    put(metadata+8,validBits);half(validBits+2,1);
    half(ids+slot*2,1);half(context+16896+8+slot*2,1);put(table+8,resource);
    put(resource+84,primary);put(resource+164,alternate);
    header(primary,alias|0x01000000);header(alternate,alias|0x02000000);header(direct,alias|0x03000000);
    if(inlineHeader) {
        for(unsigned offset:{8u,88u}) {
            put(resource+offset,1);put(resource+offset+76,0);
            header(resource+offset+4,alias|(offset==8?0x01000000:0x02000000));
        }
    }
    put(resource+172,0x10000000u|(alternateSelected?0x02000000u:0u)|(3u<<13));
    put(resource+184,memory->read32(0x82A8C560));
    for(unsigned s=0;s<16;++s) {
        base[device+11968+s]=15;
        put(device+1152+s*24+12,(1u<<19)|(1u<<21)|(1u<<23));
    }
    put(program,5);put(program+4,name);
    std::strcpy(reinterpret_cast<char*>(base+name),"UnknownNativeFragment");
    put(context+16896,program);
    put(context+17152+8,64);put(context+17152+12,32);put(context+17152+20,0x3f800000);
    EngineVertexBindingSnapshot binding;binding.deviceAddress=device;binding.descriptor.flags=0x01000000;
    binding.descriptor.modes.fill(4);
    const auto encoded=encodeEngineVertexDescriptor(binding.descriptor);
    for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
        uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
    auto geometry=std::make_shared<StoredGeometry>();
    geometry->vertexCount=3;geometry->stride=12;geometry->formats[0]=3;
    geometry->vertices.resize(36);geometry->indices={0,1,2};
    StoredDraw draw;draw.vertices=geometry;draw.indices=geometry;draw.indexCount=3;draw.vertexBindings=binding;
    return draw;
}
void bindIds(const PPCContext& initial) {
    PPCContext guest;std::memcpy(&guest,&initial,sizeof guest);guest.r3.u64=ids;guest.r4.u64=0;
    __imp__sub_822569E0(guest,memory->base());++originalCalls;
    require(guest.r1.u32==initial.r1.u32,"Original ID binder unbalanced stack");
}
void bindObject(const PPCContext& initial,unsigned slot,uint32_t object) {
    PPCContext guest;std::memcpy(&guest,&initial,sizeof guest);
    guest.r3.u64=device;guest.r4.u64=slot;guest.r5.u64=object;guest.r6.u64=uint64_t(1)<<(31-slot);
    __imp__sub_82864F20(guest,memory->base());++originalCalls;
    require(guest.r1.u32==initial.r1.u32,"Original direct binder unbalanced stack");
}
WorldDraw capture(const StoredDraw& draw,unsigned slot,uint32_t expected,const char* why) {
    auto* base=memory->base();
    const std::vector<uint8_t> before(base+fixture,base+fixture+extent),
        contextBefore(base+context,base+context+18200);
    const auto fp=_mm_getcsr();
    WorldDraw output;
    require(snapshotWorldDraw(base,draw,output),"CPU draw fixture rejected");
    require(_mm_getcsr()==fp && !std::memcmp(before.data(),base+fixture,extent) &&
        !std::memcmp(contextBefore.data(),base+context,18200),"Snapshot changed original memory or FP state");
    require(memory->read32(device+12536+slot*4)==expected,"Original completed binding differs from fixture expectation");
    if(output.textureObjects[slot].object!=expected) {
        std::fprintf(stderr,"slot=%u original=%08X snapshot=%08X\n",slot,expected,output.textureObjects[slot].object);
        require(false,why);
    }
    if(expected) {
        WorldTexture object;require(snapshotWorldTexture(base,expected,object),"Original bound header unreadable");
        require(output.textureObjects[slot].key()==object.key() && output.textureObjects[slot].width==32 &&
            output.textureObjects[slot].height==32 && output.textureObjects[slot].mipLevels==6,
            "Completed binding lost virtual storage identity or full mip dimensions");
    }
    ++comparisons;return output;
}
void oracle(const PPCContext& initial,unsigned slot,bool selected,bool inlineHeader,uint32_t alias) {
    const auto draw=setup(slot,selected,inlineHeader,alias);
    const uint32_t first=inlineHeader?resource+(selected?92:12):selected?alternate:primary;
    const uint32_t second=inlineHeader?resource+(selected?12:92):selected?primary:alternate;
    bindIds(initial);
    const auto retained=capture(draw,slot,first,"Initial original binding was not captured");
    // Crucial regression: the engine skips the resource table on an unchanged
    // texture ID. Changing the selected wrapper does not bind that wrapper.
    put(resource+172,memory->read32(resource+172)^0x02000000u);
    bindIds(initial);
    capture(draw,slot,first,"Unchanged texture ID sampled the newly selected wrapper instead of the still-bound texture");
    put(resource+176,0x7fc00000);
    capture(draw,slot,first,"A different selected wrapper's residency replaced the completed binding");
    put(resource+176,0);
    half(context+17968+slot*2,0xffff);bindIds(initial);
    capture(draw,slot,second,"Changed ID did not follow the original completed rebind");
    require(retained.textureObjects[slot].object==first,"Later rebind mutated an owned queued draw");

    bindObject(initial,slot,direct);
    capture(draw,slot,direct,"Direct device binding was replaced by the resource table");
    put(context+17964,0);
    capture(draw,slot,direct,"Completed binding depended on a mutable resource table");
    half(context+16896+8+slot*2,0);
    capture(draw,slot,direct,"Direct bound object was lost when the requested texture ID was zero");
    half(context+16896+8+slot*2,1);put(context+17964,table);
    std::array<uint8_t,24> fetch{};
    std::memcpy(fetch.data(),memory->base()+device+1152+slot*24,fetch.size());
    bindObject(initial,slot,0);
    require(!std::memcmp(fetch.data(),memory->base()+device+1152+slot*24,fetch.size()),
        "Original null binding unexpectedly cleared fetch words");
    capture(draw,slot,0,"Null completed binding revived a stale resource-table texture");

    // Streaming residency belongs to the matching resource; a reused table
    // entry must not impose another allocation's pending-prefix count.
    put(resource+176,std::bit_cast<uint32_t>(2.0f));put(resource+180,2);
    half(context+17968+slot*2,0xffff);bindIds(initial);
    auto tail=capture(draw,slot,second,"Resident original binding disappeared");
    require(tail.textureObjects[slot].firstMip==2 && tail.samplers[slot].minLevel==2,
        "Completed binding lost original streaming residency");
    put(resource+180,0);
    require(capture(draw,slot,second,"Completed streaming prefix disappeared").textureObjects[slot].firstMip==0,
        "A completed prefix retained stale residency");
    put(resource+176,0x7fc00000);
    WorldDraw invalid;require(snapshotWorldDraw(memory->base(),draw,invalid) && !invalid.textureObjects[slot].object,
        "Invalid matching residency metadata was accepted");
    put(resource+176,0);
    bindObject(initial,slot,direct);put(resource+176,0x7fc00000);
    capture(draw,slot,direct,"Unrelated resource residency rejected a directly bound texture");
    auto pooled=captureWorldDraw(memory->base(),draw);
    require(pooled && pooled->textureObjects[slot].object==direct,"Pooled capture lost direct binding");
    pooled.reset();bindObject(initial,slot,0);pooled=captureWorldDraw(memory->base(),draw);
    require(pooled && !pooled->textureObjects[slot].object,"Recycled draw revived a null texture binding");
}
void cutoutDepthCapture(const PPCContext& initial) {
    for(uint32_t colorMask:{0u,0x00100000u,0x01000000u})
        for(bool coverage:{false,true})for(unsigned comparison:{0u,1u,5u,8u}) {
            const auto source=setup(0,false,false,0);
            bindIds(initial);
            put(context+16896,0); // Original fixed-function material.
            put(context+16896+92,colorMask|6u|unsigned(coverage));
            memory->base()[context+16896+97]=uint8_t(comparison);
            WorldDraw captured;
            require(snapshotWorldDraw(memory->base(),source,captured),"Cutout depth capture rejected");
            const bool needsFragment=colorMask || coverage || comparison!=8;
            require(captured.material==(needsFragment?WorldMaterial::fixed:WorldMaterial::depth),
                    "Depth-only alpha material lost its fragment shader");
            require(captured.textureMask==(needsFragment?1:0) &&
                    captured.textureObjects[0].object==(needsFragment?primary:0),
                    "Depth-only alpha material lost its coverage texture");
            require(captured.fragmentName==(needsFragment?"MRenderXenon_Attrib_TexEnvMode01":""),
                    "Depth-only alpha material selected the wrong original program");
        }
    std::puts("CutoutDepthCapture: 24 color/depth, coverage and alpha-comparison cases passed.");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Expected game directory");
        Memory owner;memory=&owner;owner.load(argv[1]);PPCContext initial{};owner.initThread(initial);
        require(owner.commit(fixture,extent),"Cannot commit CPU fixture");
        cutoutDepthCapture(initial);
        for(unsigned slot:{0u,1u,15u})for(bool selected:{false,true})for(bool inlineHeader:{false,true})
            for(uint32_t alias:{0u,0xa0000000u,0xc0000000u,0xe0000000u})
                oracle(initial,slot,selected,inlineHeader,alias);
        std::printf("WorldTextureBindingContract passed: %llu captures, %llu original-AOT calls; cached IDs, alternating and inline wrappers, direct/null bindings, virtual aliases, residency, owned snapshots and pool reuse. CPU only.\n",comparisons,originalCalls);
        return 0;
    }catch(const std::exception& e) {
        std::fprintf(stderr,"WorldTextureBindingContract failed: %s\n",e.what());return 1;
    }
}
