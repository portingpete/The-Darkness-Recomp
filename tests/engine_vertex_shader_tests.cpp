#include "renderer/d3d11/engine_vertex_shader.h"
#include "renderer/engine/engine_vertex_program.h"
#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace DarkRecomp;
using Microsoft::WRL::ComPtr;
static void require(bool result, const char* reason) { if (!result) throw std::runtime_error(reason); }
static void checked(HRESULT result, const char* reason) { require(SUCCEEDED(result), reason); }
using Output = std::array<float,12>;
static Native::EngineVertexBindingSnapshot encodedBinding(const EngineVertexShaderConstants& constants) {
    Native::EngineVertexBindingSnapshot binding;
    auto& d=binding.descriptor;
    d.palette=uint8_t(constants.palette);d.positionConversion=uint8_t(constants.position);
    d.color=uint8_t(constants.color);d.conversions[0]=uint8_t(constants.texture);d.matrices[0]=uint8_t(constants.matrix);
    const auto descriptor=Native::encodeEngineVertexDescriptor(d);
    binding.key[0]=0x12345678;
    for(unsigned i=0;i<5;++i) for(unsigned byte=0;byte<4;++byte)
        binding.key[i+1]=(binding.key[i+1]<<8)|descriptor[i*4+byte];
    for(unsigned v=0;v<256;++v) for(unsigned lane=0;lane<4;++lane) {
        const auto word=std::bit_cast<uint32_t>(constants.vectors[v][lane]);
        for(unsigned byte=0;byte<4;++byte)binding.constantBytes[v*16+lane*4+byte]=uint8_t(word>>(24-byte*8));
    }
    return binding;
}

// Independent double-precision mathematical oracle: transform each influence
// first, then sum. The original shader instead blends matrix columns before
// transforming. This deliberately does not execute/transcribe generated HLSL.
static std::array<double,12> expected(const EngineShaderVertex& v, const EngineVertexShaderConstants& c,
                                    const EngineVertexShaderOptions& o) {
    std::array<double,4> position{}, uv{}, skinned{};
    for (unsigned lane=0; lane<4; ++lane) {
        position[lane] = o.positionConversion ? double(v.position[lane])*c.vectors[c.position][lane]+c.vectors[c.position+1][lane] : v.position[lane];
        uv[lane] = o.textureConversion ? double(v.uv[lane])*c.vectors[c.texture][lane]+c.vectors[c.texture+1][lane] : v.uv[lane];
    }
    if (o.weights) {
        for (unsigned w=0; w<o.weights; ++w) {
            const float index=w<4?v.indices[w]:v.indices2[w-4], weight=w<4?v.weights[w]:v.weights2[w-4];
            const unsigned address=unsigned(std::floor(index*c.vectors[8][3]));
            for (unsigned row=0; row<3; ++row) {
                double transformed=0;
                for (unsigned lane=0; lane<4; ++lane) transformed += double(c.vectors[c.palette+address+row][lane])*position[lane];
                skinned[row] += weight*transformed;
            }
        }
        skinned[3]=c.vectors[8][1];
    } else skinned=position;
    std::array<double,12> result{};
    for (unsigned row=0; row<4; ++row) {
        for (unsigned lane=0; lane<4; ++lane) {
            result[row] += double(c.vectors[row][lane])*(skinned[lane]+c.vectors[7][lane]);
            if (o.textureMatrix) result[4+row] += uv[lane]*c.vectors[c.matrix+row][lane];
        }
        if (!o.textureMatrix) result[4+row]=uv[row];
        result[8+row]=c.vectors[c.color][row]*(o.vertexColor?double(v.color[row]):1.0);
    }
    return result;
}

static EngineVertexShaderConstants fixture(unsigned variant) {
    EngineVertexShaderConstants c;
    // Non-default references prove these are descriptor inputs, not constants
    // guessed from one captured program. Include the final possible palette.
    c.palette=variant?100:96; c.paletteVectors=156;
    c.position=variant?24:76; c.texture=variant?26:78;
    c.matrix=variant?28:12; c.color=variant?32:10;
    for(unsigned i=0;i<c.vectors.size();++i) for(unsigned lane=0;lane<4;++lane)
        c.vectors[i][lane]=float(int((i*17+lane*7+variant*3)%29)-14)/16.0f;
    Native::EngineTransformInput transform;
    transform.modelView={{{1.2f,0.1f,0,0},{0.2f,0.8f,0.1f,0},{0,0.1f,1.5f,0},{2,-3,4,1}}};
    transform.projection={{{1.1f,0.2f,0,0},{0,1.3f,0.1f,0},{0.1f,0,0.9f,1},{0,0,-0.3f,0}}};
    Native::EngineTransformConstants prepared;
    require(Native::buildEngineTransformConstants(transform,prepared),"Original-model constant fixture failed");
    std::copy(prepared.vectors.begin(),prepared.vectors.end(),c.vectors.begin());
    c.vectors[8]={0,1,0.5f,variant?12.25f:510.0002f};
    return c;
}

static std::array<EngineShaderVertex,16> vertices(const EngineVertexShaderConstants& c) {
    std::array<EngineShaderVertex,16> values{};
    for(unsigned n=0;n<values.size();++n) {
        auto& v=values[n];
        for(unsigned lane=0;lane<4;++lane) {
            v.position[lane]=float(int(n*3+lane*5)%17-8)/8;
            v.uv[lane]=float(int(n+lane*3)%11-5)/4;
            v.color[lane]=float((n+lane)%9)/8;
            // Exercise all 52 matrices, fractional floor addresses, both input
            // sets, and non-unit/signed/zero weight sums. No normalization.
            v.indices[lane]=(float(((n*7+lane*5)%52)*3)+0.25f)/c.vectors[8][3];
            v.indices2[lane]=(float(((n*11+lane*3)%52)*3)+0.75f)/c.vectors[8][3];
            v.weights[lane]=n?float(int((n+lane*3)%9)-3)/8:0;
            v.weights2[lane]=n?float(int((n*3+lane)%7)-2)/4:0;
        }
    }
    // Read the last complete palette matrix in both sets.
    values.back().indices[3]=values.back().indices2[3]=153.25f/c.vectors[8][3];
    return values;
}

static void bounds(ID3D11Device* device, ID3D11DeviceContext* context) {
    EngineVertexShaderD3D11 shader(device,{8,true,true,true,true});
    auto c=fixture(0); auto v=vertices(c);
    require(shader.bind(context,c,v),"Valid bound fixture rejected");
    auto rejects=[&](const EngineVertexShaderConstants& bad, std::span<const EngineShaderVertex> input) {
        context->VSSetShader(nullptr,nullptr,0);
        ID3D11Buffer* nullBuffers[]{nullptr,nullptr};
        context->VSSetConstantBuffers(0,2,nullBuffers);
        context->IASetInputLayout(nullptr);
        require(!shader.bind(context,bad,input),"Invalid shader input accepted");
        ComPtr<ID3D11VertexShader> after;
        context->VSGetShader(&after,nullptr,nullptr);
        ComPtr<ID3D11Buffer> constantsAfter, referencesAfter;
        context->VSGetConstantBuffers(0,1,&constantsAfter);
        context->VSGetConstantBuffers(1,1,&referencesAfter);
        ComPtr<ID3D11InputLayout> layoutAfter;
        context->IAGetInputLayout(&layoutAfter);
        require(!after && !constantsAfter && !referencesAfter && !layoutAfter,"Rejected shader input changed D3D bindings");
    };
    for(unsigned which=0;which<8;++which) {
        auto bad=c;
        switch(which) {
            case 0:bad.palette=101;break; case 1:bad.paletteVectors=0;break;
            case 2:bad.paletteVectors=155;break; case 3:bad.position=255;break;
            case 4:bad.texture=UINT32_MAX;break; case 5:bad.matrix=253;break;
            case 6:bad.color=256;break; case 7:bad.vectors[7][0]=std::numeric_limits<float>::quiet_NaN();break;
        }
        rejects(bad,v);
    }
    for(float index:{-1.0f,154.0f/c.vectors[8][3],std::numeric_limits<float>::infinity()}) {
        auto bad=v; bad[0].indices2[3]=index; rejects(c,bad);
    }
    auto bad=v; bad[0].weights2[3]=std::numeric_limits<float>::quiet_NaN(); rejects(c,bad);
    bool rejected=false;
    try { EngineVertexShaderD3D11 invalid(device,{9}); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,"Unsupported weight count compiled");
    EngineVertexShaderD3D11 unused(device,{0});
    auto ignored=c; ignored.palette=ignored.position=ignored.texture=ignored.matrix=UINT32_MAX;
    ignored.paletteVectors=UINT32_MAX;
    bad=v; bad[0].weights2[3]=bad[0].color[0]=std::numeric_limits<float>::quiet_NaN();
    require(unused.bind(context,ignored,bad),"Unconsumed inputs were treated as shader reads");
    auto encoded=encodedBinding(c);
    EngineVertexShaderConstants imported;imported.color=123;
    encoded.key[2]^=1;
    require(!importEngineVertexShaderConstants(encoded,{8,true,true,true,true},imported) && imported.color==123,"Mismatched descriptor/key imported");
    encoded=encodedBinding(c);encoded.descriptor.positionConversion=255;
    require(!importEngineVertexShaderConstants(encoded,{8,true,true,true,true},imported) && imported.color==123,"Out-of-bank pair imported");
    encoded=encodedBinding(c);encoded.constantBytes[4*16]=0x7F;encoded.constantBytes[4*16+1]=0xC0;
    require(importEngineVertexShaderConstants(encoded,{8,true,true,true,true},imported) && imported.vectors[4][0]==0,"Unconsumed nonfloat register was uploaded");
    encoded.constantBytes[0]=0x7F;encoded.constantBytes[1]=0xC0;
    imported.color=123;
    require(!importEngineVertexShaderConstants(encoded,{8,true,true,true,true},imported) && imported.color==123,"Consumed nonfinite constant imported");
}

int main(int argc,char** argv) {
    try {
        const bool warp=argc>1 && std::strcmp(argv[1],"--warp")==0;
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        checked(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
            &level,1,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 device failed");
        std::printf("Vertex shader adapter: %s\n",warp?"WARP":"hardware");
        ComPtr<ID3D11Buffer> input, output, staging;
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=16*sizeof(EngineShaderVertex); desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        checked(device->CreateBuffer(&desc,nullptr,&input),"Input buffer failed");
        desc.ByteWidth=16*sizeof(Output); desc.BindFlags=D3D11_BIND_STREAM_OUTPUT;
        checked(device->CreateBuffer(&desc,nullptr,&output),"Stream output buffer failed");
        desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(device->CreateBuffer(&desc,nullptr,&staging),"Readback buffer failed");
        unsigned comparisons=0;
        for(unsigned weights=0;weights<=8;++weights) for(unsigned flags=0;flags<16;++flags) {
            EngineVertexShaderOptions options{weights,bool(flags&1),bool(flags&2),bool(flags&4),bool(flags&8)};
            EngineVertexShaderD3D11 shader(device.Get(),options);
            // Capture VS outputs directly, with no rasterization/pixel shader.
            // https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-creategeometryshaderwithstreamoutput
            const D3D11_SO_DECLARATION_ENTRY declaration[]={{0,"SV_Position",0,0,4,0},{0,"TEXCOORD",0,0,4,0},{0,"COLOR",0,0,4,0}};
            const UINT outputStride=sizeof(Output);
            ComPtr<ID3D11GeometryShader> stream;
            checked(device->CreateGeometryShaderWithStreamOutput(shader.bytecode()->GetBufferPointer(),shader.bytecode()->GetBufferSize(),
                declaration,3,&outputStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&stream),"VS stream capture failed");
            context->GSSetShader(stream.Get(),nullptr,0);
            for(unsigned variant=0;variant<2;++variant) {
                const auto c=fixture(variant); const auto v=vertices(c);
                const auto originalBinding=encodedBinding(c);
                EngineVertexShaderConstants imported;
                require(importEngineVertexShaderConstants(originalBinding,options,imported),"Original constant binding import failed");
                require(shader.bind(context.Get(),imported,v),"Imported shader fixture rejected");
                context->UpdateSubresource(input.Get(),0,nullptr,v.data(),0,0);
                UINT stride=sizeof(EngineShaderVertex),offset=0; ID3D11Buffer* in=input.Get(); ID3D11Buffer* out=output.Get();
                context->IASetVertexBuffers(0,1,&in,&stride,&offset);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
                context->SOSetTargets(1,&out,&offset);
                context->Draw(UINT(v.size()),0);
                context->SOSetTargets(0,nullptr,nullptr);
                context->CopyResource(staging.Get(),output.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"VS readback failed");
                std::array<Output,16> actual{}; std::memcpy(actual.data(),mapped.pData,sizeof(actual));
                context->Unmap(staging.Get(),0);
                for(unsigned n=0;n<v.size();++n) {
                    const auto reference=expected(v[n],c,options);
                    for(unsigned lane=0;lane<12;++lane) {
                        // Bounded finite fixture tolerance, not a claim of
                        // bit-exact equivalence to Xbox GPU arithmetic.
                        const double tolerance=2e-5*(1+std::abs(reference[lane]));
                        if (!std::isfinite(actual[n][lane]) || std::abs(actual[n][lane]-reference[lane])>tolerance) {
                            std::fprintf(stderr,"weights=%u flags=%u set=%u vertex=%u lane=%u actual=%.9g expected=%.12g\n",
                                weights,flags,variant,n,lane,actual[n][lane],reference[lane]);
                            throw std::runtime_error("Original-template shader output differs from independent oracle");
                        }
                        ++comparisons;
                    }
                }
            }
        }
        bounds(device.Get(),context.Get());
        context->ClearState();
        std::printf("EngineVertexShaderContract passed: 144 variants, 4608 vertices, %u float outputs; rejected invalid references/indices without binding.\n",comparisons);
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"EngineVertexShaderContract failed: %s\n",error.what()); return 1; }
}
