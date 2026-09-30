#pragma once
#include "renderer/engine/world_mesh.h"

extern "C" PPC_FUNC(__imp__sub_8223AFE8);
extern "C" PPC_FUNC(__imp__sub_8224DAE0);

// Feed the authored Water/CubeWater layouts through the actual descriptor
// builder and texture-constant uploader. Capture their completed output into
// the same owned vertex-binding format used by world draws. This exercises
// vertex preparation without executing the resource-heavy material builder.
static void testWaterVertexCapture(uint8_t* base,const PPCContext& initial) {
    PPCContext initialize;std::memcpy(&initialize,&initial,sizeof initialize);
    __imp__sub_8223AFE8(initialize,base);
    constexpr std::array<std::array<uint8_t,8>,3> layouts{{
        {{0,20,1,11,23,24,1,1}},{{0,0,8,11,23,24,1,1}},{{0,0,8,11,23,24,4,4}}
    }};
    std::array<DarkRecomp::Native::EngineVertexBindingSnapshot,6> retained{};
    unsigned generation=0;
    for(const auto& modes:layouts)for(bool matrix:{false,true}) {
        EngineTextureInput input;input.enabled=true;input.hasParameters=true;
        input.componentMask=0xFFFFFFFF;input.modes=modes;input.parameterCount=80;
        for(unsigned v=0;v<80;++v)for(unsigned lane=0;lane<4;++lane)
            input.parameters[v][lane]=std::bit_cast<uint32_t>(float(v*4+lane+generation+1)/8.f);
        if(matrix)for(unsigned s=3;s<=5;++s) {
            EngineMatrix m{};
            for(unsigned r=0;r<4;++r)for(unsigned lane=0;lane<4;++lane)
                m[r][lane]=float(s*16+r*4+lane+generation+1)/16.f;
            input.matrices[s]=m;
        }
        auto uploader=setup(base,initial,input);
        EngineVertexDescriptor state;state.flags=0x01000000|(matrix?0x3800:0);
        for(unsigned s=3;s<=5;++s)state.parameters[s][0]=uint8_t(200+s);
        const auto stateBytes=encodeEngineVertexDescriptor(state);
        std::memcpy(base+descriptor,stateBytes.data(),stateBytes.size());
        put32(base,attributes+92,0);
        PPCContext builder;std::memcpy(&builder,&initial,sizeof builder);
        builder.r3.u32=descriptor;builder.r4.u32=attributes;builder.r5.u32=pointers;builder.r6.u32=255;
        __imp__sub_8224DAE0(builder,base);
        require(memory->read32(descriptor+4)==(0x41600000u|(matrix?0x3800:0)),
                "Original water descriptor lost its normal/tangent/input flags");
        EngineTextureObservation observation;
        require(beginEngineTextureObservation(base,descriptor,cursor,pointers,attributes,observation),
                "Original Water/CubeWater constant preparation was unavailable");
        __imp__sub_8224A2E8(uploader,base);
        finishEngineTextureObservation(base,uploader.r3.u32,observation);
        require(observation.comparison==TransformComparison::equal,
                "Water constant rows/descriptor differ from original AOT uploader");
        std::array<uint8_t,80> bytes{};std::memcpy(bytes.data(),base+descriptor,bytes.size());
        auto& binding=retained[generation++];binding.deviceAddress=device;
        binding.descriptor=decodeEngineVertexDescriptor(bytes);
        for(unsigned word=0;word<5;++word)binding.key[word+1]=memory->read32(descriptor+word*4);
        for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)
            put32(binding.constantBytes.data(),row*16+lane*4,std::bit_cast<uint32_t>(row==lane?1.f:0.f));
        put32(binding.constantBytes.data(),8*16+4,0x3F800000);
        for(unsigned lane=0;lane<4;++lane)put32(binding.constantBytes.data(),10*16+lane*4,0x3F800000);
        std::memcpy(binding.constantBytes.data()+12*16,base+device+2112,uploader.r3.u32*16);
        for(unsigned s=3;s<=5;++s)for(unsigned lane=0;lane<4;++lane)
            put32(binding.constantBytes.data(),(200+s)*16+lane*4,0xFFC00000);
        WorldVertexOptions o;WorldVertexConstants c;
        require(prepareWorldVertexProgram(binding,o,c) && o.modes==modes && o.normal && o.tangents,
                "Original completed Water/CubeWater vertex binding was rejected");
        for(unsigned s=3;s<=5;++s) {
            require(o.matrices[s]==matrix && binding.descriptor.parameters[s][0]==200+s &&
                    c.vectors[200+s]==EngineVector{},"Water basis consumed a nonexistent parameter row");
            if(matrix) {
                const auto first=binding.descriptor.matrices[s];
                for(unsigned r=0;r<4;++r)for(unsigned lane=0;lane<4;++lane)
                    require(c.vectors[first+r][lane]==(*input.matrices[s])[lane][r],
                            "Original water matrix upload lost its transposed columns");
            }
        }
    }
    std::memset(base+device+2112,0xFF,112*16);std::memset(base+descriptor,0,80);
    std::memset(base+parameters,0xFF,80*16);
    for(const auto& binding:retained) {
        WorldVertexOptions o;WorldVertexConstants c;
        require(prepareWorldVertexProgram(binding,o,c) && o.modes[3]==11 && o.modes[4]==23 && o.modes[5]==24,
                "Reusing original upload/descriptor storage mutated a retained water binding");
    }
    std::puts("WaterVertexCapture: authored layouts, original AOT descriptor/constants, optional matrices and immutable vertex preparation passed.");
}
