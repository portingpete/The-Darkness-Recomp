#include "renderer/engine/engine_vertex_program.h"
#include "renderer/engine/simple_mesh.h"
#include "renderer/d3d11/engine_vertex_shader.h"
#include <bit>
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <thread>

using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_8223AFE8);
extern "C" PPC_FUNC(__imp__sub_82248C80);
constexpr uint32_t fixture=0x03000000, extent=131072, context=0x82A69B00;
constexpr uint32_t declaration=fixture+256, conversion=fixture+512, palette=fixture+1024;
constexpr uint32_t matrixBase=fixture+4096, model=matrixBase+16, node0=fixture+16384, node1=node0+64;
constexpr uint32_t device=fixture+32768, shader=fixture+65536;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static void put(uint8_t* b,uint32_t a,uint32_t v) { for(unsigned i=0;i<4;++i)b[a+i]=uint8_t(v>>(24-i*8)); }
static PPCContext clone(const PPCContext& c) { PPCContext result; std::memcpy(&result,&c,sizeof(result)); return result; }
static uint64_t cases=0,abiCases=0,words=0,bindingCases=0,bindingBytes=0,importedWords=0;
static void oracle(uint8_t* b,const PPCContext& initial,const EngineVertexProgramInput& input,unsigned treeCase,bool abi) {
    std::memset(b+fixture,0,extent); std::memset(b+context,0,18000);
    // Preserve recognizably nonzero registers that this preparation leaves
    // untouched, alongside its original palette/texture/conversion writes.
    for(unsigned vector=0;vector<256;++vector) for(unsigned lane=0;lane<4;++lane)
        put(b,device+1920+vector*16+lane*4,0x3F000000+vector*1024+lane*16);
    put(b,context+8224,matrixBase); put(b,context+15748,device);
    put(b,context+16420,input.declarationFlags); put(b,context+16424,conversion); put(b,context+16428,input.conversionMask);
    put(b,context+16528,input.declarationAddress); put(b,context+17180,input.rendererFlags);
    put(b,context+17184,input.enabledCoordinates); put(b,context+16896+92,input.materialFlags);
    put(b,model+640,input.palette?palette:0); put(b,model+644,input.modelFlags);
    b[declaration+24]=input.secondBlendIndices?17:0;
    for(unsigned s=0;s<8;++s) {
        b[context+16944+s]=input.modes[s]; b[context+16952+s]=input.coordinateSources[s];
        for(unsigned r=0;r<4;++r) put(b,model+128+s*64+r*20,0x3F800000);
    }
    // Exercise the original color constant update on some cases as well.
    put(b,context+12556,0xAF7319E1); put(b,context+17176,cases%3?0xAF7319E1:0);
    EngineVertexProgramKey expected;
    require(buildEngineVertexProgramKey(input,expected),"Valid program input rejected");
    auto record=[&](uint32_t address,const EngineVertexProgramKey& key) {
        for(unsigned i=0;i<6;++i)put(b,address+16+i*4,key[i]);
        put(b,address+8,shader);
    };
    uint32_t selected=0;
    if(treeCase!=0 && treeCase!=7) {
        put(b,context+17256,(node0+40)|1); record(node0,expected); selected=node0;
        if(treeCase==2 || treeCase==3 || treeCase==4) {
            auto other=expected; other[0]+=treeCase==3?4:uint32_t(-4); record(node0,other);
            selected=treeCase==4?0:node1;
            if(selected) {record(node1,expected);put(b,node0+(treeCase==3?40:44),(node1+40)|1);}
        }
        if(treeCase==5) {put(b,context+17188,node0);put(b,device+12688,shader);}
        if(treeCase==6) {put(b,node0,1);put(b,node0+4,shader-4);put(b,node0+8,0);}
    }
    if(treeCase==7)put(b,context+17188,node0);
    EngineVertexProgramSelection selection;
    require(lookupEngineVertexProgram(b,memory->read32(context+17256),expected,selection) && selection.recordAddress==selected &&
        selection.bindingAddress==(selected?shader:0),"Native cache lookup differs from fixture selection");
    const uint32_t stack=initial.r1.u32-8192;
    const std::vector<uint8_t> before(b+fixture,b+fixture+extent), contextBefore(b+context,b+context+18000), stackBefore(b+stack,b+stack+8320);
    const auto mode=_mm_getcsr(); EngineVertexProgramObservation observation;
    require(beginEngineVertexProgramObservation(b,initial.r1.u32,observation),"Valid program observation rejected");
    require(_mm_getcsr()==mode && !std::memcmp(b+fixture,before.data(),extent) && !std::memcmp(b+context,contextBefore.data(),18000),"Program observer changed source/FP state");
    auto original=clone(initial); __imp__sub_82248C80(original,b);
    const auto originalMode=_mm_getcsr();
    for(unsigned i=0;i<6;++i) {
        if(memory->read32(initial.r1.u32-224+i*4)!=expected[i]) {
            std::fprintf(stderr,"case%llu word%u original=%08X native=%08X\n",cases,i,memory->read32(initial.r1.u32-224+i*4),expected[i]);
            require(false,"Native key differs from original82248C80");
        }
        ++words;
    }
    require(memory->read32(context+17188)==selected,"Original cache lookup differs");
    if(selected)require(memory->read32(device+12688)==shader,"Original program binding differs");
    const std::vector<uint8_t> after(b+fixture,b+fixture+extent),contextAfter(b+context,b+context+18000),stackAfter(b+stack,b+stack+8320);
    finishEngineVertexProgramObservation(b,observation);
    require(observation.comparison==TransformComparison::equal,"Program observation did not compare equal");
    EngineVertexBindingSnapshot binding;
    require(snapshotEngineVertexBindings(b,binding)==bool(selected),"Final binding availability differs from original cache hit");
    if (selected) {
        std::optional<EngineVertexBindingSnapshot> owned;
        require(captureEngineVertexBindings(b,owned) && owned && *owned==binding,
                "Direct owned binding capture differs from the original snapshot");
        const auto descriptor=encodeEngineVertexDescriptor(binding.descriptor);
        require(binding.key==expected && binding.recordAddress==selected && binding.bindingAddress==shader &&
            binding.deviceAddress==device && binding.matrixAddress==model &&
            !std::memcmp(descriptor.data(),b+initial.r1.u32-192,80) &&
            !std::memcmp(binding.constantBytes.data(),b+device+1920,4096),"Owned final descriptor/constant bank differs from original preparation");
        ++bindingCases;bindingBytes+=4176;
        // Exercise the actual original-AOT output through the native upload
        // adapter. The explicit unlit options test transport, not key mapping.
        DarkRecomp::EngineVertexShaderConstants imported;
        require(DarkRecomp::importEngineVertexShaderConstants(binding,{},imported),"Original final constants did not import");
        for(unsigned vector : {0u,1u,2u,3u,7u,unsigned(binding.descriptor.color)})
            for(unsigned lane=0;lane<4;++lane) {
                require(std::bit_cast<uint32_t>(imported.vectors[vector][lane])==memory->read32(device+1920+vector*16+lane*4),
                    "Original-to-native constant transport changed float bits");
                ++importedWords;
            }
        if(abi) {
            // The completed descriptor must outlive its reusable guest stack.
            std::memset(b+initial.r1.u32-192,0xCD,80);
            EngineVertexBindingSnapshot again;
            require(snapshotEngineVertexBindings(b,again) && again==binding,"Guest stack reuse damaged retained descriptor");
            std::memcpy(b+initial.r1.u32-192,descriptor.data(),80);
            // Constants are read at draw time, not latched with a prior key.
            const auto previous=memory->read32(device+1920);put(b,device+1920,0x3F123456);
            require(snapshotEngineVertexBindings(b,again) && again.constantBytes[0]==0x3F &&
                again.constantBytes[1]==0x12 && again.constantBytes[2]==0x34 && again.constantBytes[3]==0x56 &&
                !std::memcmp(binding.constantBytes.data(),after.data()+(device-fixture)+1920,4096),"Draw-time constant copy failed");
            put(b,device+1920,previous);
            again=binding;put(b,device+12688,0);
            require(!captureEngineVertexBindings(b,owned) && !owned,"Failed direct capture retained stale shader constants");
            require(!snapshotEngineVertexBindings(b,again) && again==binding,"Changed device shader reused stale bindings");
            put(b,device+12688,shader);
            require(captureEngineVertexBindings(b,owned) && owned && *owned==binding,"Direct capture did not recover after shader restoration");
            const auto source=memory->read32(context+16528);put(b,context+16528,source+4);
            require(!snapshotEngineVertexBindings(b,again) && again==binding,"Changed declaration reused stale bindings");
            put(b,context+16528,source);
            DWORD protection=0,ignoredProtection=0;
            require(VirtualProtect(b+device+4096,4096,PAGE_NOACCESS,&protection),"Cannot guard constant bank page");
            const bool guarded=snapshotEngineVertexBindings(b,again);
            VirtualProtect(b+device+4096,4096,protection,&ignoredProtection);
            require(!guarded && again==binding,"Unreadable constant bank was captured or changed output");
        }
    }
    require(_mm_getcsr()==originalMode && !std::memcmp(b+fixture,after.data(),extent) && !std::memcmp(b+context,contextAfter.data(),18000),"Finishing program observation changed source/FP state");
    if(abi) {
        std::memcpy(b+fixture,before.data(),extent);std::memcpy(b+context,contextBefore.data(),18000);std::memcpy(b+stack,stackBefore.data(),stackBefore.size());
        _mm_setcsr(mode);auto wrapped=clone(initial);sub_82248C80(wrapped,b);
        require(!std::memcmp(&wrapped,&original,sizeof(wrapped)) && !std::memcmp(b+fixture,after.data(),extent) &&
            !std::memcmp(b+context,contextAfter.data(),18000) && !std::memcmp(b+stack,stackAfter.data(),stackAfter.size()) &&
            _mm_getcsr()==originalMode,"Program wrapper changed original complete ABI");++abiCases;
        EngineVertexBindingSnapshot completed;
        require(snapshotEngineVertexBindings(b,completed)==bool(selected) && (!selected || completed==binding),
                "Direct completed binding differs from independently observed original selection");
    }
    if(cases%113==0) {
        b[observation.keyAddress]^=1;finishEngineVertexProgramObservation(b,observation);
        require(observation.comparison==TransformComparison::different,"Wrong program key reported equal");
        const auto retained=observation.source;b[context+16952]^=1;finishEngineVertexProgramObservation(b,observation);
        require(observation.comparison==TransformComparison::unavailable && observation.source==retained,"Changed program source was compared or not owned");
    }
    ++cases;
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Expected game directory");Memory owner;memory=&owner;owner.load(argv[1]);PPCContext initial{};owner.initThread(initial);
        require(owner.commit(fixture,extent),"Cannot commit fixture");auto* base=owner.base();enableEnginePreview();
        auto init=clone(initial);__imp__sub_8223AFE8(init,base);
        uint32_t random=0x82248C80;auto next=[&]{random=random*1664525+1013904223;return random;};
        EngineVertexProgramInput input;input.declarationAddress=declaration;input.modes.fill(4);
        for(unsigned flags=0;flags<64;++flags) {
            input.declarationFlags=flags&1?0x400:0;input.rendererFlags=(flags&2?0x80000:0)|(flags&4?0x100000:0);
            input.palette=flags&8;input.secondBlendIndices=flags&16;input.conversionMask=flags&32?31:0;
            oracle(base,initial,input,flags%8,true);
        }
        for(unsigned slot=0;slot<8;++slot)for(unsigned value=0;value<256;++value) {
            input={};input.declarationAddress=declaration;input.modes.fill(4);input.coordinateSources[slot]=uint8_t(value);
            oracle(base,initial,input,value%8,value==255);
        }
        for(unsigned sample=0;sample<1024;++sample) {
            input={};input.declarationAddress=declaration;input.declarationFlags=next();
            input.rendererFlags=next()&0x180000;input.enabledCoordinates=next();input.conversionMask=next()&31;
            input.modelFlags=sample&1023;input.materialFlags=next();input.palette=sample&1;input.secondBlendIndices=sample&2;
            for(auto& mode:input.modes)mode=uint8_t(next()%27);
            for(auto& source:input.coordinateSources)source=uint8_t(next()>>24);
            oracle(base,initial,input,sample%8,sample%16==0);
        }
        for(auto conversionBits:{0x80000000u,0xFFFFFFE0u,0xFFFFFFFFu}) {
            input={};input.declarationAddress=declaration;input.modes.fill(4);input.conversionMask=conversionBits;
            oracle(base,initial,input,1,true);
        }
        bool otherThreadCaptured=true;
        std::thread other([&]{EngineVertexBindingSnapshot isolated;otherThreadCaptured=snapshotEngineVertexBindings(base,isolated);});other.join();
        require(!otherThreadCaptured,"Prepared vertex bindings leaked across guest threads");
        EngineVertexProgramObservation invalidObservation;invalidObservation.keyAddress=999;
        for(auto stack:{0u,16u,0x70000001u})require(!beginEngineVertexProgramObservation(base,stack,invalidObservation) && invalidObservation.keyAddress==999,"Invalid stack changed observation");
        EngineVertexProgramKey sentinel{};sentinel[0]=999;input.modes[4]=27;
        require(!buildEngineVertexProgramKey(input,sentinel) && sentinel[0]==999,"Invalid mode changed output");
        EngineVertexProgramSelection invalid;invalid.recordAddress=999;
        for(auto root:{4u,0xFFFFFFF0u})require(!lookupEngineVertexProgram(base,root,sentinel,invalid) && invalid.recordAddress==999,"Invalid tree accepted");
        std::memset(base+node0,0,48);put(base,node0+44,(node0+40)|1);
        require(!lookupEngineVertexProgram(base,node0+40,sentinel,invalid) && invalid.recordAddress==999,"Cyclic tree accepted");
        std::printf("EngineVertexProgramContract passed: %llu original-AOT cases, %llu exact key words, %llu complete wrapper ABI cases; packed mapping bytes, all modes, matrix/palette/conversion flags, cache hits/misses/tagged links/alternate binding, source ownership, bounds and FP state.\n",cases,words,abiCases);
        std::printf("Final vertex bindings: %llu original-AOT cases, %llu owned bytes, %llu bit-exact native upload words; descriptor stack reuse, draw-time constants, stale source/binding rejection and thread isolation.\n",bindingCases,bindingBytes,importedWords);
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"EngineVertexProgramContract failed: %s\n",e.what());return 1;}
}
