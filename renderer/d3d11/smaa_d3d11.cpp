#include "smaa_d3d11.h"
#include "stall_profile_d3d11.h"
#include "third_party/smaa/SMAAEmbedded.h"
#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace DarkRecomp {
namespace {
void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    char message[160];
    std::snprintf(message,sizeof(message),"SMAA %s failed: 0x%08X",operation,unsigned(result));
    throw std::runtime_error(message);
}
// Resolve the upstream shader from executable data. No shader file is required
// beside the game, and an unrelated working directory cannot change the code.
class EmbeddedInclude final : public ID3DInclude {
public:
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE,const char* name,const void*,
                                   const void** data,UINT* bytes) noexcept override {
        if(std::strcmp(name,"SMAA.hlsl")!=0)return E_FAIL;
        *data=kSmaaHlsl;*bytes=UINT(sizeof(kSmaaHlsl)-1);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(const void*) noexcept override {return S_OK;}
};
constexpr char wrapper[]=R"(
cbuffer Metrics : register(b0) { float4 renderMetrics; };
#define SMAA_RT_METRICS renderMetrics
#define SMAA_HLSL_4
#define SMAA_PRESET_HIGH
#include "SMAA.hlsl"
Texture2D colorTex : register(t0);
Texture2D edgesTex : register(t1);
Texture2D areaTex : register(t2);
Texture2D searchTex : register(t3);
Texture2D blendTex : register(t4);
struct EdgeFragment {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
};
struct WeightFragment {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
    float2 pixel : TEXCOORD4;
};
struct NeighborhoodFragment {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 offset : TEXCOORD1;
};
#if SMAA_INCLUDE_VS
float2 triangleUv(uint id) { return float2((id<<1)&2,id&2); }
float4 trianglePosition(float2 uv) { return float4(uv*float2(2,-2)+float2(-1,1),0,1); }
EdgeFragment edgeVertex(uint id:SV_VertexID) {
    EdgeFragment f;f.uv=triangleUv(id);f.position=trianglePosition(f.uv);
    SMAAEdgeDetectionVS(f.uv,f.offset);return f;
}
WeightFragment weightVertex(uint id:SV_VertexID) {
    WeightFragment f;f.uv=triangleUv(id);f.position=trianglePosition(f.uv);
    SMAABlendingWeightCalculationVS(f.uv,f.pixel,f.offset);return f;
}
NeighborhoodFragment neighborhoodVertex(uint id:SV_VertexID) {
    NeighborhoodFragment f;f.uv=triangleUv(id);f.position=trianglePosition(f.uv);
    SMAANeighborhoodBlendingVS(f.uv,f.offset);return f;
}
#endif
#if SMAA_INCLUDE_PS
float2 edgePixel(EdgeFragment f):SV_TARGET {
    return SMAAColorEdgeDetectionPS(f.uv,f.offset,colorTex);
}
float4 weightPixel(WeightFragment f):SV_TARGET {
    return SMAABlendingWeightCalculationPS(f.uv,f.pixel,f.offset,edgesTex,areaTex,searchTex,float4(0,0,0,0));
}
float4 neighborhoodPixel(NeighborhoodFragment f):SV_TARGET {
    return SMAANeighborhoodBlendingPS(f.uv,f.offset,colorTex,blendTex);
}
#endif
)";
Microsoft::WRL::ComPtr<ID3DBlob> compile(const char* entry,bool vertex) {
    const D3D_SHADER_MACRO macros[]{{"SMAA_INCLUDE_VS",vertex?"1":"0"},
                                  {"SMAA_INCLUDE_PS",vertex?"0":"1"},{nullptr,nullptr}};
    EmbeddedInclude include;
    Microsoft::WRL::ComPtr<ID3DBlob> shader,errors;
    const auto result=D3DCompile(wrapper,sizeof(wrapper)-1,"smaa_1x",macros,&include,entry,
                                vertex?"vs_5_0":"ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&shader,&errors);
    if(FAILED(result) && errors)std::fprintf(stderr,"%s\n",static_cast<const char*>(errors->GetBufferPointer()));
    check(result,entry);return shader;
}
}

SmaaD3D11::SmaaD3D11(ID3D11Device* device,ID3D11DeviceContext* context)
    :device_(device),context_(context) {
    if(!device || !context)throw std::invalid_argument("SMAA requires an initialized device");
    const char* vertices[]{"edgeVertex","weightVertex","neighborhoodVertex"};
    const char* pixels[]{"edgePixel","weightPixel","neighborhoodPixel"};
    for(unsigned index=0;index<passes_.size();++index) {
        auto& pass=passes_[index];
        const auto vertex=compile(vertices[index],true),pixel=compile(pixels[index],false);
        check(device_->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&pass.vertex),"create vertex shader");
        check(device_->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&pass.pixel),"create pixel shader");
        // Upstream declares named samplers without explicit registers. Reflect
        // each entry point: unused samplers can change the compiler's bindings.
        Ptr<ID3D11ShaderReflection> reflection;
        check(D3DReflect(pixel->GetBufferPointer(),pixel->GetBufferSize(),IID_PPV_ARGS(&reflection)),"reflect samplers");
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if(SUCCEEDED(reflection->GetResourceBindingDescByName("LinearSampler",&binding)))pass.linearSampler=binding.BindPoint;
        if(SUCCEEDED(reflection->GetResourceBindingDescByName("PointSampler",&binding)))pass.pointSampler=binding.BindPoint;
    }
    auto lookup=[&](UINT width,UINT height,DXGI_FORMAT format,const void* bytes,UINT pitch,
                    Ptr<ID3D11ShaderResourceView>& view) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;
        desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{bytes,pitch,0};Ptr<ID3D11Texture2D> texture;
        check(device_->CreateTexture2D(&desc,&data,&texture),"create lookup texture");
        check(device_->CreateShaderResourceView(texture.Get(),nullptr,&view),"create lookup view");
    };
    static_assert(sizeof(areaTexBytes)==AREATEX_SIZE && sizeof(searchTexBytes)==SEARCHTEX_SIZE);
    lookup(AREATEX_WIDTH,AREATEX_HEIGHT,DXGI_FORMAT_R8G8_UNORM,areaTexBytes,AREATEX_PITCH,area_);
    lookup(SEARCHTEX_WIDTH,SEARCHTEX_HEIGHT,DXGI_FORMAT_R8_UNORM,searchTexBytes,SEARCHTEX_PITCH,search_);
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    check(device_->CreateSamplerState(&sampler,&linear_),"create linear sampler");
    sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    check(device_->CreateSamplerState(&sampler,&point_),"create point sampler");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=16;buffer.Usage=D3D11_USAGE_DYNAMIC;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    check(device_->CreateBuffer(&buffer,nullptr,&metrics_),"create metrics");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
    raster.DepthClipEnable=TRUE;check(device_->CreateRasterizerState(&raster,&raster_),"create rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    check(device_->CreateDepthStencilState(&depth,&depth_),"create depth state");
}

SmaaD3D11::Image SmaaD3D11::image(UINT width,UINT height,DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.Format=format;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    Image result;check(device_->CreateTexture2D(&desc,nullptr,&result.texture),"create intermediate image");
    check(device_->CreateRenderTargetView(result.texture.Get(),nullptr,&result.target),"create intermediate target");
    check(device_->CreateShaderResourceView(result.texture.Get(),nullptr,&result.source),"create intermediate source");
    return result;
}
void SmaaD3D11::ensureTargets(UINT width,UINT height) {
    if(width==width_ && height==height_)return;
    // Commit only when all three allocations and their views are ready.
    auto edges=image(width,height,DXGI_FORMAT_R8G8_UNORM);
    auto weights=image(width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto output=image(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT);
    edges_=std::move(edges);weights_=std::move(weights);output_=std::move(output);
    width_=width;height_=height;
}
ID3D11ShaderResourceView* SmaaD3D11::filter(ID3D11ShaderResourceView* source) {
    if(!source)throw std::invalid_argument("SMAA source is missing");
    Ptr<ID3D11Resource> resource;source->GetResource(&resource);
    Ptr<ID3D11Texture2D> texture;check(resource.As(&texture),"get source texture");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1)throw std::invalid_argument("SMAA requires resolved input");
    ensureTargets(desc.Width,desc.Height);
    context_->ClearState();
    const float zero[4]{};
    context_->ClearRenderTargetView(edges_.target.Get(),zero);
    context_->ClearRenderTargetView(weights_.target.Get(),zero);
    const float metrics[]{1.0f/width_,1.0f/height_,float(width_),float(height_)};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(stallProfileMap(context_.Get(),metrics_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped,"SMAA metrics Map"),"map metrics");
    std::memcpy(mapped.pData,metrics,sizeof(metrics));context_->Unmap(metrics_.Get(),0);
    ID3D11Buffer* constant=metrics_.Get();
    context_->VSSetConstantBuffers(0,1,&constant);context_->PSSetConstantBuffers(0,1,&constant);
    context_->RSSetState(raster_.Get());context_->OMSetDepthStencilState(depth_.Get(),0);
    const D3D11_VIEWPORT viewport{0,0,float(width_),float(height_),0,1};context_->RSSetViewports(1,&viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11RenderTargetView* targets[]{edges_.target.Get(),weights_.target.Get(),output_.target.Get()};
    const std::array<std::array<ID3D11ShaderResourceView*,5>,3> sources{{
        {source,nullptr,nullptr,nullptr,nullptr},
        {nullptr,edges_.source.Get(),area_.Get(),search_.Get(),nullptr},
        {source,nullptr,nullptr,nullptr,weights_.source.Get()}
    }};
    for(unsigned index=0;index<passes_.size();++index) {
        const auto& pass=passes_[index];
        context_->OMSetRenderTargets(1,&targets[index],nullptr);
        context_->VSSetShader(pass.vertex.Get(),nullptr,0);context_->PSSetShader(pass.pixel.Get(),nullptr,0);
        if(pass.linearSampler<D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
            ID3D11SamplerState* sampler=linear_.Get();context_->PSSetSamplers(pass.linearSampler,1,&sampler);
        }
        if(pass.pointSampler<D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
            ID3D11SamplerState* sampler=point_.Get();context_->PSSetSamplers(pass.pointSampler,1,&sampler);
        }
        context_->PSSetShaderResources(0,UINT(sources[index].size()),sources[index].data());
        context_->Draw(3,0);
        ID3D11ShaderResourceView* empty[5]{};context_->PSSetShaderResources(0,5,empty);
        context_->OMSetRenderTargets(0,nullptr,nullptr);
    }
    return output_.source.Get();
}
}
