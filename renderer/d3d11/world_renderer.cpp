#include "world_renderer.h"
#include "world_render_state.h"
#include "runtime/native/graphics_settings.h"
#include "renderer/engine/prompt_icons.h"
#include "renderer/engine/engine_performance.h"
#include "renderer/engine/render_trace.h"
#include <d3d11shader.h>
#include "engine_world_template.generated.h"
#include "engine_world_fragments.generated.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <set>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace DarkRecomp {
namespace {
void check(HRESULT hr,const char* op) {if (FAILED(hr)) throw std::runtime_error(std::string(op)+": "+std::to_string(hr));}
uint32_t word(const uint8_t* b) {return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];}
double millisBetween(std::chrono::steady_clock::time_point begin,std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double,std::milli>(end-begin).count();
}
// McDonald GDC12: UpdateSubresource costs more CPU than Map/Unmap on dynamic
// buffers. Constant updates below stay gated on content changes exactly as
// before; only the upload mechanism changes to DISCARD mapping.
void updateConstants(ID3D11DeviceContext* context,ID3D11Buffer* buffer,const void* data,size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(buffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"world constant map");
    std::memcpy(mapped.pData,data,bytes);
    context->Unmap(buffer,0);
}
// Proven normal-independent output modes (audit38): texcoord0, linear1,
// void4, constant7, mspos8, constant-9 passthrough, wspos10 and distance attenuation
// in mode16 never move R9 (or the R0/R1 tangent basis) into an output.
// Modes13/17/18/20/22 do, so they stay strict. Unknown modes stay strict;
// template stage gates still apply at generation time.
bool normalIndependentDrawModes(const Native::WorldVertexOptions& o) {
    for (auto m:o.modes) switch (m) {
        case 0:case 1:case 4:case 7:case 8:case 9:case 10:case 16:break;
        default:return false;
    }
    return true;
}
// Views are used only for lookup; every inserted key owns all of its bytes.
template<class Cache> typename Cache::mapped_type& cacheEntry(Cache& cache,std::string_view key) {
    if(auto found=cache.find(key);found!=cache.end())return found->second;
    return cache.try_emplace(std::string(key)).first->second;
}
std::array<char,6+8*4> shaderKey(const Native::WorldVertexOptions& o) {
    std::array<char,6+8*4> key{};
    size_t next=0;
    for (auto x:{o.weights,uint32_t(o.positionConversion),uint32_t(o.normal),uint32_t(o.tangents),
                 uint32_t(o.normalizeNormal),uint32_t(o.vertexColor)}) key[next++]=char(x);
    for (unsigned i=0;i<8;++i) {key[next++]=char(o.modes[i]);key[next++]=char(o.coordinates[i]);
        key[next++]=char(o.conversions[i]);key[next++]=char(o.matrices[i]);}return key;
}
// FNV-1a over owned bytes for smoke-evidence sampling. Inspection-only;
// never called on normal runs.
uint64_t smokeHashBytes(const void* data,size_t size,uint64_t seed=14695981039346656037ull) {
    const auto* bytes=static_cast<const uint8_t*>(data);
    uint64_t hash=seed;
    for(size_t i=0;i<size;++i) {hash^=bytes[i];hash*=1099511628211ull;}
    return hash;
}
}
namespace {
std::filesystem::path worldVertexShaderCacheDir() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,executable,MAX_PATH);
    try {
        if(length && length<MAX_PATH) {
            auto dir=std::filesystem::path(executable).parent_path()/"DarkRecompShaderCache";
            std::error_code status;
            std::filesystem::create_directories(dir,status);
            if(!status) return dir;
        }
        auto fallback=std::filesystem::temp_directory_path()/"DarkRecompShaderCache";
        std::error_code status;
        std::filesystem::create_directories(fallback,status);
        if(!status) return fallback;
    } catch (...) {}
    return {};
}
uint64_t worldVertexShaderCacheKey(const Native::WorldVertexOptions& o,bool rasterize) {
    // FNV-1a over the compiled template source, rasterize variant and packed
    // options. Any template or flag change yields a different filename, so a
    // stale cache entry can never shadow a new permutation.
    uint64_t hash=14695981039346656037ull;
    auto mix=[&](const void* data,size_t size) {
        const auto* bytes=static_cast<const uint8_t*>(data);
        for(size_t i=0;i<size;++i) {hash^=bytes[i];hash*=1099511628211ull;}
    };
    constexpr uint64_t version=1;
    mix(&version,sizeof(version));
    mix(engineWorldTemplateSource,std::strlen(engineWorldTemplateSource));
    mix(&rasterize,sizeof(rasterize));
    mix(&o.weights,sizeof(o.weights));
    mix(&o.positionConversion,sizeof(o.positionConversion));
    mix(&o.normal,sizeof(o.normal));
    mix(&o.tangents,sizeof(o.tangents));
    mix(&o.normalizeNormal,sizeof(o.normalizeNormal));
    mix(&o.vertexColor,sizeof(o.vertexColor));
    mix(o.modes.data(),o.modes.size());
    mix(o.coordinates.data(),o.coordinates.size());
    mix(o.conversions.data(),o.conversions.size()*sizeof(bool));
    mix(o.matrices.data(),o.matrices.size()*sizeof(bool));
    return hash;
}
bool loadCachedWorldVertexShader(ID3D11Device* device,uint64_t key,Microsoft::WRL::ComPtr<ID3DBlob>& code) {
    const auto dir=worldVertexShaderCacheDir();
    if(dir.empty()) return false;
    char name[64]{};
    std::snprintf(name,sizeof(name),"worldvs_v1_%016llX.cso",key);
    std::error_code status;
    const auto size=std::filesystem::file_size(dir/name,status);
    if(status || !size || size>1024*1024) return false;
    std::ifstream file(dir/name,std::ios::binary);
    if(!file) return false;
    std::vector<uint8_t> bytes;
    bytes.resize(static_cast<size_t>(size));
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if(FAILED(D3DCreateBlob(bytes.size(),&blob))) return false;
    std::memcpy(blob->GetBufferPointer(),bytes.data(),bytes.size());
    Microsoft::WRL::ComPtr<ID3D11VertexShader> probe;
    if(FAILED(device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&probe)))
        return false;
    code=std::move(blob);
    return true;
}
void saveCachedWorldVertexShader(uint64_t key,ID3DBlob* code) {
    if(!code || !code->GetBufferSize() || code->GetBufferSize()>1024*1024) return;
    try {
        const auto dir=worldVertexShaderCacheDir();
        if(dir.empty()) return;
        char name[64]{}, temporary[80]{};
        std::snprintf(name,sizeof(name),"worldvs_v1_%016llX.cso",key);
        std::snprintf(temporary,sizeof(temporary),"worldvs_v1_%016llX.%lu.tmp",key,GetCurrentProcessId());
        const auto target=dir/name, staging=dir/temporary;
        {
            std::ofstream file(staging,std::ios::binary|std::ios::trunc);
            if(!file) return;
            file.write(static_cast<const char*>(code->GetBufferPointer()),code->GetBufferSize());
            if(!file) {std::error_code ignored;std::filesystem::remove(staging,ignored);return;}
        }
        std::error_code renameStatus;
        std::filesystem::rename(staging,target,renameStatus);
        if(renameStatus) {std::error_code ignored;std::filesystem::remove(staging,ignored);}
    } catch (...) {}
}
}
WorldVertexShaderD3D11::WorldVertexShaderD3D11(ID3D11Device* device,const Native::WorldVertexOptions& o,bool rasterize):options_(o) {
    if (!device || o.weights>8) throw std::invalid_argument("Invalid world vertex options");
    const uint64_t cacheKey=worldVertexShaderCacheKey(o,rasterize);
    if(loadCachedWorldVertexShader(device,cacheKey,code_)) {
        check(device->CreateVertexShader(code_->GetBufferPointer(),code_->GetBufferSize(),nullptr,&shader_),"world VS (cached)");
    } else {
        std::vector<std::pair<std::string,std::string>> definitions{
            {"MWCOMP",std::to_string(o.weights)},{"POSITION_TRANS",std::to_string(o.positionConversion)},
            {"USE_NORMAL",std::to_string(o.normal)},{"USE_TANGENTS",std::to_string(o.tangents)},
            {"NORMALIZE_NORMAL",std::to_string(o.normalizeNormal)},{"VERTEX_COLOR",std::to_string(o.vertexColor)}};
        for (unsigned i=0;i<8;++i) {
            const auto s=std::to_string(i);
            definitions.insert(definitions.end(),{{"MODE_"+s,std::to_string(o.modes[i])},{"COORD_"+s,std::to_string(o.coordinates[i])},
                {"CONVERT_"+s,std::to_string(o.conversions[i])},{"MATRIX_"+s,std::to_string(o.matrices[i])}});
        }
        std::vector<D3D_SHADER_MACRO> macros;
        for (const auto& [k,v]:definitions) macros.push_back({k.c_str(),v.c_str()});macros.push_back({nullptr,nullptr});
        Ptr<ID3DBlob> errors;
        // Preserve the original shader output; apply the engine's viewport depth
        // range afterward. 82246F20 initializes this range to (1,0).
        std::string source=engineWorldTemplateSource;
        if(rasterize)source+=R"(
cbuffer NativeViewport : register(b2) {float4 nativeDepthRange;};
VertexOutput rasterMain(VertexInput input) {
    VertexOutput result=vertexMain(input);
    result.position.z=nativeDepthRange.x*result.position.w+(nativeDepthRange.y-nativeDepthRange.x)*result.position.z;
    return result;
})";
        const auto compileStart=std::chrono::steady_clock::now();
        const auto hr=D3DCompile(source.data(),source.size(),"original_world_VP",macros.data(),nullptr,
            rasterize?"rasterMain":"vertexMain","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_IEEE_STRICTNESS,0,&code_,&errors);
        if (FAILED(hr)) throw std::runtime_error(errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"World vertex compilation failed");
        const double compileMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compileStart).count();
        // A synchronous compile blocks the render thread. Log only hitch-sized
        // misses so steady 60 FPS runs stay quiet after the cache warms up.
        static std::atomic<unsigned> slowCompiles{};
        if(compileMs>8.0 && slowCompiles.fetch_add(1,std::memory_order_relaxed)<16)
            std::fprintf(stderr,"[RenderShaderCompile] worldVS miss key=%016llX ms=%.1f weights=%u\n",
                cacheKey,compileMs,o.weights);
        check(device->CreateVertexShader(code_->GetBufferPointer(),code_->GetBufferSize(),nullptr,&shader_),"world VS");
        saveCachedWorldVertexShader(cacheKey,code_.Get());
    }
    using V=Native::WorldVertex;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    auto element=[&](const char* semantic,unsigned index,size_t offset) {elements.push_back({semantic,index,DXGI_FORMAT_R32G32B32A32_FLOAT,0,UINT(offset),D3D11_INPUT_PER_VERTEX_DATA,0});};
    element("POSITION",0,offsetof(V,position));element("NORMAL",0,offsetof(V,normal));
    for (unsigned s=0;s<8;++s) element("TEXCOORD",s,offsetof(V,tex)+s*16);
    element("COLOR",0,offsetof(V,color));element("BLENDINDICES",0,offsetof(V,indices));element("BLENDWEIGHT",0,offsetof(V,weights));
    element("BLENDINDICES",1,offsetof(V,indices2));element("BLENDWEIGHT",1,offsetof(V,weights2));
    check(device->CreateInputLayout(elements.data(),UINT(elements.size()),code_->GetBufferPointer(),code_->GetBufferSize(),&layout_),"world layout");
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=4096;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    desc.Usage=D3D11_USAGE_DYNAMIC;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&desc,nullptr,&constants_),"world constants");desc.ByteWidth=144;
    check(device->CreateBuffer(&desc,nullptr,&references_),"world references");
}
bool WorldVertexShaderD3D11::bind(ID3D11DeviceContext* context,const Native::WorldVertexConstants& c,const std::vector<Native::WorldVertex>& vertices,
                               const std::array<std::array<float,2>,8>* indexBounds,bool alreadyBound) {
    if (!context) return false;
    auto valid=[&](float index) {
        const float address=std::floor(index*c.vectors[8][3]);
        return std::isfinite(address) && address>=0 && address<=153 && uint64_t(c.references[0][0])+unsigned(address)+2<256;
    };
    if(indexBounds) {
        // Multiplication by a fixed scale and floor are monotone (with order
        // reversed for a negative scale). Validate the immutable stream's two
        // extrema instead of rescanning every vertex for every material pass.
        for(unsigned w=0;w<options_.weights;++w)
            if(!valid((*indexBounds)[w][0]) || !valid((*indexBounds)[w][1]))return false;
    } else for(const auto& v:vertices)for(unsigned w=0;w<options_.weights;++w)
        if(!valid(w<4?v.indices[w]:v.indices2[w-4]))return false;
    const bool initial=!uploadedConstants_;
    if(initial)uploadedConstants_.emplace();
    if(initial || std::memcmp(uploadedConstants_->vectors.data(),c.vectors.data(),sizeof(c.vectors))) {
        updateConstants(context,constants_.Get(),c.vectors.data(),sizeof(c.vectors));
        uploadedConstants_->vectors=c.vectors;
    }
    if(initial || std::memcmp(uploadedConstants_->references.data(),c.references.data(),sizeof(c.references))) {
        updateConstants(context,references_.Get(),c.references.data(),sizeof(c.references));
        uploadedConstants_->references=c.references;
    }
    if(!alreadyBound) {
        ID3D11Buffer* buffers[]{constants_.Get(),references_.Get()};context->VSSetConstantBuffers(0,2,buffers);
        context->IASetInputLayout(layout_.Get());context->VSSetShader(shader_.Get(),nullptr,0);
    }
    return true;
}
WorldRendererD3D11::WorldRendererD3D11(ID3D11Device* d,ID3D11DeviceContext* c,uint32_t scale):device_(d),context_(c),scale_(scale),transientBuffers_(d,c) {
    if(scale<1 || scale>3)throw std::invalid_argument("World render scale must be between one and three");
    Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if(SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) && desc.DedicatedVideoMemory)
        imageBudget_=std::clamp(size_t(desc.DedicatedVideoMemory/4),size_t(256*1024*1024),size_t(2ull*1024*1024*1024));
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    if(GlobalMemoryStatusEx(&memory))imageBudget_=(std::min)(imageBudget_,(std::max)(size_t(256*1024*1024),size_t(memory.ullAvailPhys/4)));
    auto compile=[&](const char* source,const char* name,Ptr<ID3D11PixelShader>& shader) {
        Ptr<ID3DBlob> code,errors;
        const auto hr=D3DCompile(source,std::strlen(source),name,nullptr,nullptr,"pixelMain","ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&errors);
        if (FAILED(hr)) throw std::runtime_error(errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Fragment compile failed");
        check(d->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),name);
        return code;
    };
    for(const auto& source:worldFragmentSources) {
        FragmentProgram program;
        // 82247FE8 maps attribute97 through82066AFC and normalizes the
        // 16-bit reference at98 by1/255. Preserve the original fragment,
        // then perform the fixed-function alpha comparison before blending.
        std::string nativeSource="#define pixelMain originalPixelMain\n";
        nativeSource+=source.source;
        nativeSource+=R"(
#undef pixelMain
cbuffer NativeAlphaTest : register(b2) {uint alphaFunction;float alphaReference;uint2 alphaPadding;};
float4 pixelMain(Fragment input) : SV_Target {
    float4 color=originalPixelMain(input);
    bool alphaPasses=alphaFunction==8 ||
        (alphaFunction==2 && color.a<alphaReference) ||
        (alphaFunction==3 && color.a==alphaReference) ||
        (alphaFunction==4 && color.a<=alphaReference) ||
        (alphaFunction==5 && color.a>alphaReference) ||
        (alphaFunction==6 && color.a!=alphaReference) ||
        (alphaFunction==7 && color.a>=alphaReference);
    if(!alphaPasses)discard;
    return color;
}
)";
        const auto binary=compile(nativeSource.c_str(),source.name,program.shader);
        Ptr<ID3D11ShaderReflection> reflection;
        check(D3DReflect(binary->GetBufferPointer(),binary->GetBufferSize(),IID_PPV_ARGS(&reflection)),"fragment reflection");
        D3D11_SHADER_DESC description{};check(reflection->GetDesc(&description),"fragment description");
        // Original sources issue optional fetches before their conditionals.
        // Require only resources surviving compilation into this permutation.
        for(unsigned i=0;i<description.BoundResources;++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding{};check(reflection->GetResourceBindingDesc(i,&binding),"fragment resource");
            if(binding.Type!=D3D_SIT_TEXTURE)continue;
            if(binding.BindPoint>=16 || binding.BindCount!=1 ||
               (binding.Dimension!=D3D_SRV_DIMENSION_TEXTURE2D && binding.Dimension!=D3D_SRV_DIMENSION_TEXTURECUBE))
                throw std::runtime_error("Unsupported compiled original texture binding");
            const unsigned mask=1u<<binding.BindPoint;
            if(!(source.textures&mask) || bool(source.cubes&mask)!=(binding.Dimension==D3D_SRV_DIMENSION_TEXTURECUBE))
                throw std::runtime_error("Compiled texture binding differs from original source");
            program.textures|=mask;
            if(binding.Dimension==D3D_SRV_DIMENSION_TEXTURECUBE)program.cubes|=mask;
        }
        const auto captureMask=Native::worldFragmentTextureMask(source.name,source.flags);
        if(captureMask!=source.textures || (program.textures&~captureMask))
            throw std::runtime_error("Native capture omits a compiled original texture binding");
        fragments_.emplace(std::string(source.name)+":"+std::to_string(source.flags),std::move(program));
    }
    constexpr char transferSource[]=R"(
cbuffer Transfer : register(b0) {float4 area;float4 originScale;};
float4 vertexMain(uint id:SV_VertexID):SV_Position {
    return float4(id==2?3:-1,id==1?3:-1,0,1);
}
Texture2D<float4> sourceImage:register(t0);
float4 pixelMain(float4 p:SV_Position):SV_Target {
    return sourceImage.Load(int3(int2(p.xy-originScale.xy+area.xy),0))*originScale.z;
})";
    Ptr<ID3DBlob> code,errors;
    check(D3DCompile(transferSource,sizeof(transferSource)-1,"engine_resolve_copy",nullptr,nullptr,"vertexMain","vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"resolve VS compilation");
    check(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&transferVertex_),"resolve VS");
    compile(transferSource,"engine_resolve_copy",transferPixel_);
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    check(d->CreateDepthStencilState(&depth,&transferDepth_),"resolve depth state");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    check(d->CreateRasterizerState(&raster,&transferRaster_),"resolve raster state");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=4096;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    buffer.Usage=D3D11_USAGE_DYNAMIC;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    check(d->CreateBuffer(&buffer,nullptr,&fragmentConstants_),"fragment constants");
    buffer.ByteWidth=256;check(d->CreateBuffer(&buffer,nullptr,&textureScales_),"texture scales");
    buffer.ByteWidth=16;check(d->CreateBuffer(&buffer,nullptr,&viewportConstants_),"viewport constants");
    check(d->CreateBuffer(&buffer,nullptr,&alphaTestConstants_),"alpha test constants");
    buffer.ByteWidth=32;check(d->CreateBuffer(&buffer,nullptr,&transferConstants_),"resolve constants");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    check(d->CreateSamplerState(&sampler,&wrapSampler_),"world wrap sampler");
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    check(d->CreateSamplerState(&sampler,&cubeSampler_),"world cube sampler");
}
ID3D11ShaderResourceView* WorldRendererD3D11::image(const std::shared_ptr<const ColorImage>& source) {
    if (!source || !source->valid())return nullptr;
    const unsigned levels=std::bit_width((std::max)(source->width,source->height));
    if (auto it=images_.find(source.get());it!=images_.end()) {it->second.used=++resourceUse_;return it->second.view.Get();}
    const auto imageStart=std::chrono::steady_clock::now();
    D3D11_TEXTURE2D_DESC desc{};desc.Width=source->width;desc.Height=source->height;
    desc.MipLevels=source->authoredMips?unsigned(source->mips.size()+1):levels;desc.ArraySize=source->faces;
    desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if(!source->authoredMips) {desc.BindFlags|=D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;}
    if (source->faces==6) desc.MiscFlags|=D3D11_RESOURCE_MISC_TEXTURECUBE;
    Image result;result.source=source;result.used=++resourceUse_;
    for(unsigned mip=0;mip<desc.MipLevels;++mip)
        result.bytes+=size_t((std::max)(1u,source->width>>mip))*(std::max)(1u,source->height>>mip)*source->faces*4;
    while(!images_.empty() && imageBytes_+result.bytes>imageBudget_) {
        auto oldest=std::min_element(images_.begin(),images_.end(),[](const auto& l,const auto& r){return l.second.used<r.second.used;});
        imageBytes_-=oldest->second.bytes;images_.erase(oldest);
    }
    Ptr<ID3D11Texture2D> texture;check(device_->CreateTexture2D(&desc,nullptr,&texture),"world image");
    for(unsigned mip=source->firstMip;mip<=source->mips.size();++mip) {
        const auto& pixels=mip?source->mips[mip-1]:source->pixels;
        const unsigned width=(std::max)(1u,source->width>>mip),height=(std::max)(1u,source->height>>mip);
        for(unsigned f=0;f<source->faces;++f)context_->UpdateSubresource(texture.Get(),D3D11CalcSubresource(mip,f,desc.MipLevels),nullptr,
            pixels.data()+size_t(f)*width*height*4,width*4,0);
    }
    check(device_->CreateShaderResourceView(texture.Get(),nullptr,&result.view),"world image view");
    if(!source->authoredMips)context_->GenerateMips(result.view.Get());
    imageBytes_+=result.bytes;++imageUploads_;
    noteWorstUpload(millisBetween(imageStart,std::chrono::steady_clock::now()),result.bytes,"image");
    return images_.emplace(source.get(),std::move(result)).first->second.view.Get();
}
WorldRendererD3D11::Surface& WorldRendererD3D11::surface(uint64_t key,uint32_t width,uint32_t height,bool depth) {
    auto& s=surfaces_[key];
    if (s.texture && s.width>=width && s.height>=height) return s;
    const auto surfaceStart=std::chrono::steady_clock::now();
    const auto old=s;
    width=(std::max)(old.width,width);height=(std::max)(old.height,height);
    if(!width || !height || width>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/scale_ || height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION/scale_)
        throw std::invalid_argument("Scaled world surface exceeds GPU dimensions");
    s={};s.width=width;s.height=height;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width*scale_;desc.Height=height*scale_;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=depth?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags=(depth?D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET)|D3D11_BIND_SHADER_RESOURCE;
    check(device_->CreateTexture2D(&desc,nullptr,&s.texture),"world surface");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Format=depth?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:desc.Format;
    check(device_->CreateShaderResourceView(s.texture.Get(),&srv,&s.view),"world surface read view");
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;dsv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
        check(device_->CreateDepthStencilView(s.texture.Get(),&dsv,&s.depth),"world depth view");
        context_->ClearDepthStencilView(s.depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0,0);
    } else {
        check(device_->CreateRenderTargetView(s.texture.Get(),nullptr,&s.color),"world color view");
        const float zero[4]{};context_->ClearRenderTargetView(s.color.Get(),zero);
    }
    if(old.texture) {
        const D3D11_BOX box{0,0,0,old.width*scale_,old.height*scale_,1};
        context_->OMSetRenderTargets(0,nullptr,nullptr);
        invalidateBindings();
        if(depth) {
            // D3D11 requires equal extents and whole-resource copies whenever
            // a depth/stencil-bound texture participates. Expand between two
            // unbound typeless textures so all packed D24S8 bits survive, with
            // the newly allocated border retaining its cleared zero values.
            D3D11_TEXTURE2D_DESC copyDesc=desc;copyDesc.BindFlags=0;
            Ptr<ID3D11Texture2D> expanded,previous;
            check(device_->CreateTexture2D(&copyDesc,nullptr,&expanded),"expanded depth copy");
            copyDesc.Width=old.width*scale_;copyDesc.Height=old.height*scale_;
            check(device_->CreateTexture2D(&copyDesc,nullptr,&previous),"previous depth copy");
            context_->CopyResource(previous.Get(),old.texture.Get());
            context_->CopyResource(expanded.Get(),s.texture.Get());
            context_->CopySubresourceRegion(expanded.Get(),0,0,0,0,previous.Get(),0,&box);
            context_->CopyResource(s.texture.Get(),expanded.Get());
        } else context_->CopySubresourceRegion(s.texture.Get(),0,0,0,0,old.texture.Get(),0,&box);
    }
    noteWorstUpload(millisBetween(surfaceStart,std::chrono::steady_clock::now()),
        size_t(desc.Width)*desc.Height*(depth?4:8),depth?"surface-depth":"surface-color");
    return s;
}
void WorldRendererD3D11::transfer(ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target,
                                  const std::array<uint32_t,4>& rectangle,const std::array<uint32_t,2>& offset,float scale) {
    invalidateBindings();
    std::array<ID3D11ShaderResourceView*,16> empty{};context_->PSSetShaderResources(0,16,empty.data());
    context_->OMSetRenderTargets(1,&target,nullptr);context_->OMSetDepthStencilState(transferDepth_.Get(),0);
    context_->OMSetBlendState(nullptr,nullptr,~0u);context_->RSSetState(transferRaster_.Get());
    // Both ends of a resolve have the same integer scale. Keep the Load shader
    // in physical pixels; the guest rectangle (including its fringe) is intact.
    const D3D11_VIEWPORT viewport{float(offset[0]*scale_),float(offset[1]*scale_),float((rectangle[2]-rectangle[0])*scale_),float((rectangle[3]-rectangle[1])*scale_),0,1};
    context_->RSSetViewports(1,&viewport);
    const float constants[]{float(rectangle[0]*scale_),float(rectangle[1]*scale_),float(rectangle[2]*scale_),float(rectangle[3]*scale_),float(offset[0]*scale_),float(offset[1]*scale_),scale,0};
    updateConstants(context_.Get(),transferConstants_.Get(),constants,sizeof(constants));
    ID3D11Buffer* buffer=transferConstants_.Get();context_->PSSetConstantBuffers(0,1,&buffer);
    context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(transferVertex_.Get(),nullptr,0);context_->PSSetShader(transferPixel_.Get(),nullptr,0);
    context_->GSSetShader(nullptr,nullptr,0);context_->HSSetShader(nullptr,nullptr,0);context_->DSSetShader(nullptr,nullptr,0);
    context_->PSSetShaderResources(0,1,&source);
    // An engine resolve copies pixels; its native helper triangle is not
    // geometry contributing to the original exposure histogram.
    const bool histogramPaused=pauseHistogram();context_->Draw(3,0);
    if(histogramPaused)resumeHistogram();
    context_->PSSetShaderResources(0,16,empty.data());context_->OMSetRenderTargets(0,nullptr,nullptr);
}
bool WorldRendererD3D11::resolve(const Native::WorldResolve& r) {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::resolve);
    const unsigned attachment=r.flags&7;const bool depth=attachment==4;
    auto reject=[&](unsigned reason,unsigned sourceWidth=0,unsigned sourceHeight=0) {
        static const bool probe=std::getenv("DARK_EFFECT_PROBE")!=nullptr;
        static std::array<unsigned,8> reports{};
        constexpr const char* causes[]{"unused","descriptor","source-missing","depth-format",
            "format","empty-rectangle","source-extent","destination-extent"};
        // Normal user runs need the reason for a failed scene copy too. Keep
        // separate, bounded budgets so a repeated early failure cannot hide a
        // later execution-effect failure of a different kind.
        if(reports[reason]<(probe?32u:8u)) {
            ++reports[reason];
            std::fprintf(stderr,"[ResolveEvidence] reason=%u cause=%s present=%llu flags=%08X source=%08X sourceSize=%ux%u destination=%08X storage=%08X destinationSize=%ux%u viewport=%u,%u,%u,%u rectangle=%u,%u,%u,%u offset=%u,%u format=%u face=%u mip=%u surfaceKey=%016llX layout=%08X info=%08X\n",
                reason,causes[reason],static_cast<unsigned long long>(presentCount_),r.flags,
                attachment<5?r.targets[attachment]:0,sourceWidth,sourceHeight,
                r.destination.object,r.destination.storage,r.destination.width,r.destination.height,
                r.viewport[0],r.viewport[1],r.viewport[2],r.viewport[3],r.rectangle[0],r.rectangle[1],
                r.rectangle[2],r.rectangle[3],r.offset[0],r.offset[1],r.destination.format,r.face,r.mip,
                static_cast<unsigned long long>(r.surfaceKey(attachment)),
                attachment<5?r.surfaceBindings[attachment].layout:0,attachment<5?r.surfaceBindings[attachment].info:0);
        }
        return false;
    };
    if(attachment>4 || !r.destination.object || !r.destination.width || !r.destination.height ||
       r.destination.width>4096 || r.destination.height>4096 || r.mip ||
       (r.destination.faces!=1 && r.destination.faces!=6) || r.face>=r.destination.faces) return reject(1);
    auto src=surfaces_.find(r.surfaceKey(attachment));
    if(src==surfaces_.end())return reject(2);
    if(r.rectangle[0]>=r.rectangle[2] || r.rectangle[1]>=r.rectangle[3])
        return reject(5,src->second.width,src->second.height);
    if(r.rectangle[2]>src->second.width || r.rectangle[3]>src->second.height)
        return reject(6,src->second.width,src->second.height);
    if(uint64_t(r.offset[0])+r.rectangle[2]-r.rectangle[0]>r.destination.width ||
       uint64_t(r.offset[1])+r.rectangle[3]-r.rectangle[1]>r.destination.height)
        return reject(7,src->second.width,src->second.height);
    DXGI_FORMAT format;
    switch(r.destination.format) {
        case 6:case 54:format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
        case 26:format=DXGI_FORMAT_R16G16B16A16_UNORM;break;
        case 23:if(!depth)return reject(3);format=DXGI_FORMAT_R32_FLOAT;break;
        default:return reject(4);
    }
    auto& dest=resolved_[r.destination.key()];
    D3D11_TEXTURE2D_DESC existing{};if(dest.texture)dest.texture->GetDesc(&existing);
    if(dest.width!=r.destination.width || dest.height!=r.destination.height || existing.Format!=format || existing.ArraySize!=r.destination.faces) {
        dest={};dest.width=r.destination.width;dest.height=r.destination.height;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=dest.width*scale_;desc.Height=dest.height*scale_;desc.MipLevels=desc.SampleDesc.Count=1;desc.ArraySize=r.destination.faces;
        if(desc.ArraySize==6)desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE;
        desc.Format=format;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        check(device_->CreateTexture2D(&desc,nullptr,&dest.texture),"resolved texture");
        if(desc.ArraySize==1)check(device_->CreateRenderTargetView(dest.texture.Get(),nullptr,&dest.color),"resolved target");
        else for(unsigned f=0;f<6;++f) {
            D3D11_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=format;rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray.FirstArraySlice=f;rtv.Texture2DArray.ArraySize=1;
            check(device_->CreateRenderTargetView(dest.texture.Get(),&rtv,&dest.faces[f]),"resolved cube face");
        }
        check(device_->CreateShaderResourceView(dest.texture.Get(),nullptr,&dest.view),"resolved view");
        const float zero[4]{};
        if(dest.color)context_->ClearRenderTargetView(dest.color.Get(),zero);
        else for(auto& face:dest.faces)context_->ClearRenderTargetView(face.Get(),zero);
    }
    const std::array<uint32_t,4> resolveRegion{r.rectangle[0],r.rectangle[1],
        (r.rectangle[2]+7u)&~7u,(r.rectangle[3]+7u)&~7u};
    auto copied=resolveRegion;
    // Round and clip in logical pixels before transfer scales the coordinates.
    // The guest copy's expanded fringe can extend into allocation padding.
    copied[2]=uint32_t((std::min)({uint64_t(resolveRegion[2]),uint64_t(src->second.width),
        uint64_t(r.rectangle[0])+r.destination.width-r.offset[0]}));
    copied[3]=uint32_t((std::min)({uint64_t(resolveRegion[3]),uint64_t(src->second.height),
        uint64_t(r.rectangle[1])+r.destination.height-r.offset[1]}));
    transfer(src->second.view.Get(),r.destination.faces==6?dest.faces[r.face].Get():dest.color.Get(),copied,r.offset,std::exp2(float(r.exponent)));
    auto clearSource=[&](unsigned slot,const Surface& target,bool depth) {
        Native::WorldClear reset;reset.flags=depth?48:1;reset.targets[depth?4:0]=r.targets[slot];
        reset.surfaceBindings[depth?4:0]=r.surfaceBindings[slot];
        reset.viewport={0,0,target.width,target.height};reset.color=r.color;reset.depth=r.depth;reset.stencil=r.stencil;
        // Original82865FD0 expands right/bottom to eight pixels before its
        // resolve draw. Clear only that source region, clipped to each retained
        // attachment, without resizing it or shifting by destination offset.
        reset.rectangle=std::array<int32_t,4>{int32_t(resolveRegion[0]),int32_t(resolveRegion[1]),
            int32_t(resolveRegion[2]),int32_t(resolveRegion[3])};
        clear(reset);
    };
    if((r.flags&0x100) && !depth)clearSource(attachment,src->second,false);
    if((r.flags&0x200) && r.targets[4]) {
        auto it=surfaces_.find(r.surfaceKey(4));
        if(it!=surfaces_.end())clearSource(4,it->second,true);
    }
    return true;
}
bool WorldRendererD3D11::present(const Native::WorldTexture& texture,ID3D11Texture2D* target) {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::copy);
    const auto it=resolved_.find(texture.key());if(it==resolved_.end() || !target)return false;
    D3D11_TEXTURE2D_DESC src{},dest{};it->second.texture->GetDesc(&src);target->GetDesc(&dest);
    if(src.Width!=dest.Width || src.Height!=dest.Height || src.Format!=dest.Format)return false;
    context_->OMSetRenderTargets(0,nullptr,nullptr);invalidateBindings();context_->CopyResource(target,it->second.texture.Get());
    ++presentCount_;inspectFrame_=false;return true;
}
void WorldRendererD3D11::histogram(const std::shared_ptr<Native::WorldQuery>& query,bool begin) {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::queries);
    if(!query || !query->result)throw std::invalid_argument("Missing histogram query owner");
    if(begin) {
        if(activeHistogram_.source)throw std::invalid_argument("Nested histogram query");
        activeHistogram_.source=query;
        D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};
        check(device_->CreateQuery(&desc,&activeHistogram_.query),"histogram query");
        context_->Begin(activeHistogram_.query.Get());
    } else {
        if(activeHistogram_.source!=query)throw std::invalid_argument("Unmatched histogram query end");
        context_->End(activeHistogram_.query.Get());
        pendingHistograms_.push_back(std::move(activeHistogram_));activeHistogram_={};
    }
}
bool WorldRendererD3D11::pauseHistogram() {
    if(!activeHistogram_.source)return false;
    context_->End(activeHistogram_.query.Get());
    activeHistogram_.segments.push_back(std::move(activeHistogram_.query));return true;
}
void WorldRendererD3D11::resumeHistogram() {
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION,0};
    check(device_->CreateQuery(&desc,&activeHistogram_.query),"resumed histogram query");
    context_->Begin(activeHistogram_.query.Get());
}
void WorldRendererD3D11::pollHistograms() {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::queries);
    // Keep completion ordered: an older pending measurement must never replace
    // a newer result. No CPU estimate or synthetic completion is published.
    size_t completed=0;
    for(auto& query:pendingHistograms_) {
        uint64_t samples=0;
        auto collect=[&](ID3D11Query* part) {
            uint64_t value=0;
            const auto hr=context_->GetData(part,&value,sizeof(value),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE)return false;
            check(hr,"histogram result");
            if(UINT64_MAX-samples<value)throw std::overflow_error("Histogram sample sum overflow");
            samples+=value;return true;
        };
        bool ready=true;
        for(auto& part:query.segments)if(!collect(part.Get())) {ready=false;break;}
        if(!ready || !collect(query.query.Get()))break;
        // Guest exposure consumes logical pixel counts. Normalize after summing
        // all segments so helper-induced query splits cannot lose extra samples.
        query.source->result->samples.store(samples/(scale_*scale_),std::memory_order_release);++completed;
    }
    pendingHistograms_.erase(pendingHistograms_.begin(),pendingHistograms_.begin()+completed);
}
void WorldRendererD3D11::initializePartialClear() {
    if(clearReady_)return;
    constexpr char source[]=R"(
cbuffer ClearConstants : register(b0) {float4 clearColor;float4 clearDepth;};
float4 clearVS(uint id : SV_VertexID) : SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),clearDepth.x,1);
}
float4 clearPS() : SV_Target {return clearColor;}
)";
    auto compile=[&](const char* entry,const char* profile) {
        Ptr<ID3DBlob> code,error;
        const auto hr=D3DCompile(source,sizeof(source)-1,"native_rectangular_clear",nullptr,nullptr,entry,profile,
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
        if(FAILED(hr))throw std::runtime_error(error?std::string(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()):"Clear shader compilation failed");
        return code;
    };
    const auto vs=compile("clearVS","vs_5_0"),ps=compile("clearPS","ps_5_0");
    check(device_->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&clearVertex_),"clear VS");
    check(device_->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&clearPixel_),"clear PS");
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=32;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    cb.Usage=D3D11_USAGE_DYNAMIC;cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    check(device_->CreateBuffer(&cb,nullptr,&clearConstants_),"clear constants");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
    raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
    check(device_->CreateRasterizerState(&raster,&clearRaster_),"clear rasterizer");
    for(unsigned mask=0;mask<4;++mask) {
        D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=(mask&1)!=0;depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
        depth.DepthWriteMask=(mask&1)?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.StencilEnable=(mask&2)!=0;depth.StencilReadMask=depth.StencilWriteMask=255;
        depth.FrontFace={D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};
        depth.BackFace=depth.FrontFace;
        check(device_->CreateDepthStencilState(&depth,&clearDepthStates_[mask]),"clear depth/stencil state");
    }
    clearReady_=true;
}
void WorldRendererD3D11::clear(const Native::WorldClear& c) {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::clear);
    const auto width=c.viewport[0]+c.viewport[2],height=c.viewport[1]+c.viewport[3];
    bool initialized=false,histogramPaused=false;
    auto region=[&](Surface& target,bool depth) {
        D3D11_RECT rect{0,0,LONG(target.width),LONG(target.height)};
        if(c.rectangle) {
            const auto& r=*c.rectangle;
            rect={(std::max)(LONG(0),LONG(r[0])),(std::max)(LONG(0),LONG(r[1])),
                  (std::min)(LONG(target.width),LONG(r[2])),(std::min)(LONG(target.height),LONG(r[3]))};
        }
        if(rect.right<=rect.left || rect.bottom<=rect.top) {++emptyClearRegions_;return;}
        if(rect.left==0 && rect.top==0 && rect.right==LONG(target.width) && rect.bottom==LONG(target.height)) {
            if(depth)context_->ClearDepthStencilView(target.depth.Get(),((c.flags&16)?D3D11_CLEAR_DEPTH:0)|((c.flags&32)?D3D11_CLEAR_STENCIL:0),std::clamp(c.depth,0.0f,1.0f),UINT8(c.stencil));
            else context_->ClearRenderTargetView(target.color.Get(),c.color.data());
            ++fullClearRegions_;return;
        }
        if(!initialized) {
            initializePartialClear();invalidateBindings();
            // Clear helper geometry must not contribute to the engine's
            // occlusion-based exposure histogram. Resume the query afterward.
            histogramPaused=pauseHistogram();
            std::array<ID3D11ShaderResourceView*,16> empty{};context_->PSSetShaderResources(0,16,empty.data());
            const std::array<Native::EngineVector,2> constants{c.color,Native::EngineVector{std::clamp(c.depth,0.0f,1.0f),0,0,0}};
            updateConstants(context_.Get(),clearConstants_.Get(),constants.data(),sizeof(constants));
            ID3D11Buffer* buffer=clearConstants_.Get();context_->VSSetConstantBuffers(0,1,&buffer);context_->PSSetConstantBuffers(0,1,&buffer);
            context_->IASetInputLayout(nullptr);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(clearVertex_.Get(),nullptr,0);
            context_->GSSetShader(nullptr,nullptr,0);context_->HSSetShader(nullptr,nullptr,0);context_->DSSetShader(nullptr,nullptr,0);
            context_->RSSetState(clearRaster_.Get());context_->OMSetBlendState(nullptr,nullptr,~0u);initialized=true;
        }
        rect.left*=LONG(scale_);rect.top*=LONG(scale_);rect.right*=LONG(scale_);rect.bottom*=LONG(scale_);
        const D3D11_VIEWPORT viewport{0,0,float(target.width*scale_),float(target.height*scale_),0,1};
        context_->RSSetViewports(1,&viewport);context_->RSSetScissorRects(1,&rect);
        auto* color=target.color.Get();
        context_->OMSetRenderTargets(depth?0:1,depth?nullptr:&color,depth?target.depth.Get():nullptr);
        context_->OMSetDepthStencilState(clearDepthStates_[depth?((c.flags>>4)&3):0].Get(),UINT8(c.stencil));
        context_->PSSetShader(depth?nullptr:clearPixel_.Get(),nullptr,0);
        context_->Draw(3,0);++partialClearRegions_;
    };
    // Separate attachments can have different retained extents. Clearing each
    // independently avoids invalid MRT bindings and preserves unrelated pixels.
    if(c.flags&1)for(unsigned t=0;t<4;++t)if(c.targets[t])region(surface(c.surfaceKey(t),width,height,false),false);
    if((c.flags&48) && c.targets[4])region(surface(c.surfaceKey(4),width,height,true),true);
    if(histogramPaused)resumeHistogram();
}
std::vector<uint8_t> WorldRendererD3D11::readSurface(uint32_t identity,bool depth) {
    return readSurfaceKey(Native::WorldSurfaceBinding::syntheticKey(identity,depth),depth);
}
std::vector<uint8_t> WorldRendererD3D11::readSurface(const Native::WorldSurfaceTargets& targets,unsigned slot) {
    return readSurfaceKey(targets.surfaceKey(slot),slot==4);
}
std::vector<uint8_t> WorldRendererD3D11::readSurfaceKey(uint64_t key,bool depth) {
    const auto it=surfaces_.find(key);
    if (it==surfaces_.end()) return {};
    const auto& s=it->second;D3D11_TEXTURE2D_DESC desc{};s.texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Ptr<ID3D11Texture2D> staging;check(device_->CreateTexture2D(&desc,nullptr,&staging),"world readback texture");
    context_->CopyResource(staging.Get(),s.texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mapResult=context_->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
    if(FAILED(mapResult))check(device_->GetDeviceRemovedReason(),"world readback device removal");
    check(mapResult,"world readback map");
    const size_t row=size_t(desc.Width)*(depth?4:8);std::vector<uint8_t> result(row*desc.Height);
    for (unsigned y=0;y<desc.Height;++y) std::memcpy(result.data()+row*y,static_cast<const uint8_t*>(mapped.pData)+size_t(mapped.RowPitch)*y,row);
    context_->Unmap(staging.Get(),0);return result;
}
void WorldRendererD3D11::noteWorstUpload(double ms, size_t bytes, const char* kind) {
    if (ms > worstUploadMs_) { worstUploadMs_ = ms; worstUploadBytes_ = bytes; worstUploadKind_ = kind; }
}
bool WorldRendererD3D11::draw(const Native::WorldDraw& draw) {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::textures);
    if(draw.fragmentName=="XREngine_RadialBlurInvert") {
        static const bool probe=std::getenv("DARK_EFFECT_PROBE")!=nullptr;
        static unsigned reports=0;
        if(probe && reports<4) {
            Native::traceOwnedWorldDraw(draw,1,reports++,true);
            std::fprintf(stderr,"[DarknessEffect] viewport=%u,%u,%u,%u target=%08X textures=%u,%u,%u,%u\n",
                draw.viewport[0],draw.viewport[1],draw.viewport[2],draw.viewport[3],draw.targets[0],
                draw.textureIds[0],draw.textureIds[1],draw.textureIds[2],draw.textureIds[3]);
        }
    }
    using Clock=std::chrono::steady_clock;
    const bool profile=Native::profileEngineCpu;
    Clock::time_point phase{};
    if(profile)phase=Clock::now();
    // Task-54 smoke-evidence ordinal: deterministic per draw() call while an
    // explicit inspection is active; stays zero on normal runs.
    unsigned smokeOrdinal=0;
    if(inspectFrame_) {
        smokeOrdinal=smokeDrawOrdinal_++;
        if(inspection_<=16 && smokeOrdinal<512)
            Native::traceOwnedWorldDraw(draw,inspection_,smokeOrdinal);
    }
    auto timing=[&](double& sum){
        if(!profile)return;
        const auto now=Clock::now();sum+=std::chrono::duration<double,std::milli>(now-phase).count();phase=now;
    };
    // Page-in pacing: defer fresh decode/upload past the per-frame budget.
    // Callers return rejected(12); the draw reappears via later snapshots.
    auto budgetFor=[&](size_t estimate) {
        if (frameUploadBytes_ > 0 && frameUploadBytes_ + estimate > kFrameUploadBudgetBytes) {
            ++budgetDeferred_;
            return false;
        }
        return true;
    };
    auto rejected=[&](unsigned step) {
        static std::set<std::string> reports;
        const auto key=draw.fragmentName+":"+std::to_string(draw.fragmentFlags)+":"+std::to_string(step);
        if(reports.size()<64 && reports.insert(key).second) {
            std::fprintf(stderr,"[EngineWorldRejectReason] step=%u program=%s flags=%u material=%u vptr=%p formats=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u modes=%u,%u,%u,%u,%u,%u,%u,%u coords=%u,%u,%u,%u,%u,%u,%u,%u weights=%u normal=%u tangents=%u normalize=%u color=%u\n",step,
            draw.fragmentName.c_str(),draw.fragmentFlags,unsigned(draw.material),static_cast<const void*>(draw.geometry.vertices.get()),
            draw.geometry.vertices?draw.geometry.vertices->formats[0]:0,draw.geometry.vertices?draw.geometry.vertices->formats[1]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[2]:0,draw.geometry.vertices?draw.geometry.vertices->formats[3]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[4]:0,draw.geometry.vertices?draw.geometry.vertices->formats[5]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[6]:0,draw.geometry.vertices?draw.geometry.vertices->formats[7]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[8]:0,draw.geometry.vertices?draw.geometry.vertices->formats[9]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[10]:0,draw.geometry.vertices?draw.geometry.vertices->formats[11]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[12]:0,draw.geometry.vertices?draw.geometry.vertices->formats[13]:0,
            draw.geometry.vertices?draw.geometry.vertices->formats[14]:0,draw.geometry.vertices?draw.geometry.vertices->formats[15]:0,
            draw.options.modes[0],draw.options.modes[1],draw.options.modes[2],draw.options.modes[3],
            draw.options.modes[4],draw.options.modes[5],draw.options.modes[6],draw.options.modes[7],
            draw.options.coordinates[0],draw.options.coordinates[1],draw.options.coordinates[2],draw.options.coordinates[3],
            draw.options.coordinates[4],draw.options.coordinates[5],draw.options.coordinates[6],draw.options.coordinates[7],
            draw.options.weights,unsigned(draw.options.normal),unsigned(draw.options.tangents),unsigned(draw.options.normalizeNormal),unsigned(draw.options.vertexColor));
            if(step==1 && draw.fragmentName=="XREngine_ShadowProj" && draw.fragmentFlags==8) {
                std::fprintf(stderr,"[EngineShadowEvidence] viewport=%u,%u,%u,%u targets=%08X,%08X,%08X,%08X,%08X textureMask=%04X depthRange=%g,%g,%g,%g program=%s flags=%u\n",
                    unsigned(draw.viewport[0]),unsigned(draw.viewport[1]),unsigned(draw.viewport[2]),unsigned(draw.viewport[3]),
                    unsigned(draw.targets[0]),unsigned(draw.targets[1]),unsigned(draw.targets[2]),unsigned(draw.targets[3]),unsigned(draw.targets[4]),
                    unsigned(draw.textureMask),
                    double(draw.depthRange[0]),double(draw.depthRange[1]),double(draw.depthRange[2]),double(draw.depthRange[3]),
                    draw.fragmentName.c_str(),unsigned(draw.fragmentFlags));
                for(unsigned slot=0;slot<2;++slot) {
                    const auto& texture=draw.textureObjects[slot];
                    const auto& sampled=draw.samplers[slot];
                    const auto& cpu=draw.textures[slot];
                    const bool hasCpu=cpu!=nullptr;
                    const auto found=resolved_.find(texture.key());
                    const bool hasResolved=found!=resolved_.end();
                    unsigned resolvedWidth=0,resolvedHeight=0,srvFormat=0,srvDim=0;
                    if(hasResolved) {
                        resolvedWidth=unsigned(found->second.width);resolvedHeight=unsigned(found->second.height);
                        if(found->second.view) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                            found->second.view->GetDesc(&desc);
                            srvFormat=unsigned(desc.Format);srvDim=unsigned(desc.ViewDimension);
                        }
                    }
                    std::fprintf(stderr,"[EngineShadowEvidence] slot=%u texId=%u object=%08X storage=%08X key=%016llX size=%ux%u format=%u exponent=%d faces=%u mipLevels=%u firstMip=%u sampler=%u,%u,%u,%u,%u,%u addr=%u,%u,%u anisotropy=%u minLevel=%u maxLevel=%u border=%u bias=%g cpu=%u cpuSize=%ux%u cpuFaces=%u cpuFirstMip=%u cpuMips=%u resolved=%u resolvedSize=%ux%u srvFormat=%u srvDim=%u\n",
                        slot,unsigned(draw.textureIds[slot]),
                        unsigned(texture.object),unsigned(texture.storage),static_cast<unsigned long long>(texture.key()),
                        unsigned(texture.width),unsigned(texture.height),unsigned(texture.format),texture.exponent,
                        unsigned(texture.faces),unsigned(texture.mipLevels),unsigned(texture.firstMip),
                        unsigned(sampled.valid),unsigned(sampled.minLinear),unsigned(sampled.magLinear),unsigned(sampled.mipLinear),
                        unsigned(sampled.baseOnly),unsigned(sampled.lodValid),
                        unsigned(sampled.address[0]),unsigned(sampled.address[1]),unsigned(sampled.address[2]),
                        unsigned(sampled.anisotropy),unsigned(sampled.minLevel),unsigned(sampled.maxLevel),
                        unsigned(sampled.border),double(sampled.bias),
                        unsigned(hasCpu),hasCpu?unsigned(cpu->width):0u,hasCpu?unsigned(cpu->height):0u,
                        hasCpu?unsigned(cpu->faces):0u,hasCpu?unsigned(cpu->firstMip):0u,hasCpu?unsigned(cpu->mips.size()):0u,
                        unsigned(hasResolved),resolvedWidth,resolvedHeight,srvFormat,srvDim);
                }
                for(unsigned env=0;env<10;++env)
                    std::fprintf(stderr,"[EngineShadowEvidence] env%u=%g,%g,%g,%g\n",env,
                        double(draw.fragmentConstants[env][0]),double(draw.fragmentConstants[env][1]),
                        double(draw.fragmentConstants[env][2]),double(draw.fragmentConstants[env][3]));
            }
        }
        return false;
    };
    for (unsigned m=0;m<5;++m) if (queryPending_[m]) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        if (context_->GetData(queries_[m].Get(),&stats,sizeof(stats),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK) {
            std::fprintf(stderr,"[EngineWorldGPU] material=%u draw=%llu IAvertices=%llu IAtriangles=%llu VS=%llu clipperIn=%llu clipperOut=%llu PS=%llu\n",
                m,queryOrdinals_[m],stats.IAVertices,stats.IAPrimitives,stats.VSInvocations,stats.CInvocations,stats.CPrimitives,stats.PSInvocations);
            queryPending_[m]=false;
        }
    }
    const char* fallback=draw.material==Native::WorldMaterial::motion?"XRShader_MotionMap":
        draw.material==Native::WorldMaterial::ndsp?"XRShader_FP20_NDSP":"";
    // Avoid a per-draw heap allocation: build the permutation key on the stack
    // and use the transparent fragment cache for lookup. Fragment names are
    // bounded (original 96-byte names) so a fixed buffer cannot overflow.
    // Consecutive draws usually repeat the previous material: then reuse the
    // cached key/program instead of reformatting and rehashing the key.
    const char* effectiveName=draw.fragmentName.empty()?fallback:draw.fragmentName.c_str();
    const unsigned fragmentFlags=Native::graphicsFragmentFlags(effectiveName,draw.fragmentFlags,bloom_,motionBlur_);
    const FragmentProgram* frag = nullptr;
    bool fragFound = false;
    if (lastFragmentValid_ && lastFragmentFlags_ == fragmentFlags && lastFragmentName_ == effectiveName) {
        frag = lastFragment_; fragFound = lastFragmentFound_;
    } else {
        char fragmentKey[128]{};
        const int fragmentKeyLength=std::snprintf(fragmentKey,sizeof(fragmentKey),"%s:%u",
            effectiveName,fragmentFlags);
        if(fragmentKeyLength<=0 || size_t(fragmentKeyLength)>=sizeof(fragmentKey)) return rejected(1);
        lastFragmentKey_.assign(fragmentKey,size_t(fragmentKeyLength));
        const auto found=fragments_.find(std::string_view(lastFragmentKey_));
        fragFound = found != fragments_.end();
        frag = fragFound ? &found->second : nullptr;
        lastFragmentName_ = effectiveName; lastFragmentFlags_ = fragmentFlags;
        lastFragment_ = frag; lastFragmentFound_ = fragFound; lastFragmentValid_ = true;
    }
    if(draw.material!=Native::WorldMaterial::depth && !fragFound) return rejected(1);
    std::array<ID3D11ShaderResourceView*,16> views{};std::array<ID3D11SamplerState*,16> samplers{};
    // Selecting a later slot can evict an earlier slot from either cache.
    // Retain each selection until the context has acquired its own reference.
    std::array<Ptr<ID3D11ShaderResourceView>,16> retainedViews;
    std::array<Ptr<ID3D11SamplerState>,16> retainedSamplers;
    std::array<Native::EngineVector,16> scales{};for(auto& scale:scales)scale.fill(1);
    if(fragFound) for(unsigned slot=0;slot<16;++slot)if(frag->textures&(1u<<slot)) {
        const auto& texture=draw.textureObjects[slot];const auto resolved=resolved_.find(texture.key());
        const bool cube=(frag->cubes&(1u<<slot))!=0;
        unsigned firstMip=0;
        if(texture.object && resolved!=resolved_.end()) {
            if(cube!=(texture.faces==6) || texture.width!=resolved->second.width || texture.height!=resolved->second.height) return rejected(2);
            views[slot]=resolved->second.view.Get();
        } else {
            // Owned prompt origin selects a cached icon without mutating the
            // draw snapshot. Controller source, unknown origins, cubemaps and
            // resolved render-targets keep the original image. Replacement
            // icons are single-level, so the sampler is reset to a base-only
            // default instead of reusing an original partial-mip LOD range.
            std::shared_ptr<const ColorImage> effective = draw.textures[slot];
            bool promptSubstituted = false;
            if (promptKeyboardMouse_ && effective && effective->faces == 1 && !cube && effective->promptOrigin) {
                if (auto icon = Prompts::replacementFor(draw.textureIds[slot], effective,
                        Prompts::Source::KeyboardMouse, Prompts::Context::Menu)) {
                    effective = std::move(icon);
                    promptSubstituted = true;
                    static std::atomic<unsigned> promptLogs{0};
                    if (promptLogs++ < 8)
                        std::fprintf(stderr, "[PromptSubstitute] program=%s slot=%u id=%u origin=%u\n",
                            lastFragmentKey_.c_str(), slot, draw.textureIds[slot],
                            unsigned(draw.textures[slot]->promptOrigin));
                }
            }
            if(!effective || effective->faces!=(cube?6u:1u) || !(views[slot]=image(effective))) {
                static std::set<uint32_t> missing;
                if(missing.size()<64 && missing.insert(draw.textureIds[slot]).second) std::fprintf(stderr,"[EngineWorldMissingTexture] program=%s slot=%u id=%u object=%08X storage=%08X format=%u size=%ux%u faces=%u\n",
                    lastFragmentKey_.c_str(),slot,draw.textureIds[slot],texture.object,texture.storage,texture.format,texture.width,texture.height,texture.faces);
                return rejected(3);
            }
            if (promptSubstituted) {
                firstMip = 0;
                samplers[slot] = (cube || draw.material==Native::WorldMaterial::post ||
                    draw.material==Native::WorldMaterial::fixed) ? cubeSampler_.Get() : wrapSampler_.Get();
                scales[slot].fill(std::exp2(float(texture.exponent)));
                retainedViews[slot]=views[slot];retainedSamplers[slot]=samplers[slot];
                continue;
            }
        }
        if(!(texture.object && resolved!=resolved_.end()))firstMip=draw.textures[slot]->firstMip;
        auto captured=draw.samplers[slot];
        if(captured.valid) {
            if(captured.maxLevel<firstMip)return rejected(3);
            captured.minLevel=(std::max)(captured.minLevel,uint8_t(firstMip));
            samplers[slot]=sampler(captured);
        }
        else if(firstMip || captured.lodValid) {
            Native::WorldSampler fallback;
            fallback.minLinear=fallback.magLinear=fallback.mipLinear=true;
            fallback.address.fill(cube || draw.material==Native::WorldMaterial::post ||
                draw.material==Native::WorldMaterial::fixed?2:0);
            fallback.minLevel=uint8_t(firstMip);fallback.maxLevel=15;
            if(captured.lodValid) {
                if(captured.maxLevel<firstMip)return rejected(3);
                fallback.minLevel=(std::max)(fallback.minLevel,captured.minLevel);
                fallback.maxLevel=captured.maxLevel;fallback.baseOnly=captured.baseOnly;
                fallback.bias=captured.bias;
            }
            // Keep existing fallback addressing/filtering without sampling an
            // uninitialized prefix or discarding independently valid LOD limits.
            samplers[slot]=sampler(fallback);
        } else samplers[slot]=(cube || draw.material==Native::WorldMaterial::post ||
            draw.material==Native::WorldMaterial::fixed)?cubeSampler_.Get():wrapSampler_.Get();
        // Fetch scaling belongs to the binding, including CPU-owned images.
        scales[slot].fill(std::exp2(float(texture.exponent)));
        retainedViews[slot]=views[slot];retainedSamplers[slot]=samplers[slot];
    }
    timing(profileTextures_);
    Native::setRenderSamplePhase(Native::RenderSamplePhase::geometry);
    const auto* a=draw.attributes.data();const auto flags=word(a+92);
    if (!draw.geometry || ((flags&(2|0x4000)) && !draw.targets[4]) ||
        (draw.material!=Native::WorldMaterial::depth && !draw.targets[0])) return rejected(4);
    const auto& g=draw.geometry; const auto count=g.indexCount;
    if (!count || g.firstIndex>g.indices->indices.size() || count>g.indices->indices.size()-g.firstIndex) return rejected(5);
    const auto& formats=g.vertices->formats;const auto& options=draw.options;
    // A missing normal stream is dead prologue data when no live output can
    // consume R9 and no skinning/tangent/normalize stage can observe it. The
    // shader still compiles with USE_NORMAL; missing slots already decode to
    // finite (0,0,0,1). Anything else keeps the original strict rejection.
    const bool missingNormal=options.normal && !formats[9];
    const bool deadNormal=missingNormal && !options.weights && !options.tangents &&
        !options.normalizeNormal && normalIndependentDrawModes(options);
    if ((missingNormal && !deadNormal) || (options.vertexColor && !formats[10]) ||
        (options.weights && (!formats[12] || !formats[13])) ||
        (options.weights>4 && (!formats[14] || !formats[15]))) return rejected(6);
    for (unsigned s=0;s<8;++s) {
        const bool textureInput=options.modes[s]==0 || options.modes[s]==13 || options.modes[s]==22;
        const bool tangentInput=options.tangents && (s==2 || s==3);
        if((textureInput || tangentInput) && options.coordinates[s]>=8)return rejected(7);
        // Original82871600 links an absent tangent semantic to constant zero
        // XYZ (cache record13), rather than a different UV stream. Tangents
        // consume XYZ only; decodeWorldVertices already owns that default.
        if(textureInput && !formats[options.coordinates[s]+1])return rejected(7);
    }
    auto it=geometry_.find(g.vertices.get());
    if (it==geometry_.end()) {
        // Index bytes are budgeted separately at their own miss below.
        // Transient immediate snapshots bypass the budget (see header).
        const bool storedUpload = (g.vertices->address != 0 || g.vertices->id != 0);
        const size_t estimate=size_t(g.vertices->vertexCount)*sizeof(Native::WorldVertex);
        if (storedUpload && !budgetFor(estimate)) return rejected(12);
        Geometry native;native.source=g.vertices;
        const auto decodeStart=Clock::now();
        if (!Native::decodeWorldVertices(*g.vertices,native.vertices)) return rejected(8);
        for(unsigned w=0;w<8;++w) {
            auto value=[&](const auto& v){return w<4?v.indices[w]:v.indices2[w-4];};
            auto& bounds=native.indexBounds[w];bounds.fill(value(native.vertices.front()));
            for(const auto& v:native.vertices) {bounds[0]=(std::min)(bounds[0],value(v));bounds[1]=(std::max)(bounds[1],value(v));}
        }
        const size_t decodedBytes=native.vertices.size()*sizeof(Native::WorldVertex);
        const double decodeMs=millisBetween(decodeStart,Clock::now());
        if (decodeMs > worstDecodeMs_) { worstDecodeMs_=decodeMs; worstDecodeBytes_=decodedBytes; }
        const auto uploadStart=Clock::now();
        D3D11_BUFFER_DESC b{};b.ByteWidth=UINT(native.vertices.size()*sizeof(Native::WorldVertex));b.BindFlags=D3D11_BIND_VERTEX_BUFFER;b.Usage=D3D11_USAGE_IMMUTABLE;
        if(!g.vertices->address && !g.vertices->id)
            native.transientCapacity=transientBuffers_.upload(native.vertices.data(),b.ByteWidth,b.BindFlags,native.buffer);
        else {
            D3D11_SUBRESOURCE_DATA data{native.vertices.data(),0,0};check(device_->CreateBuffer(&b,&data,&native.buffer),"world VB");
        }
        const size_t bytes=native.bytes();
        // Evict only the least recently used resource. Clearing the whole
        // cache at128 entries reuploaded the opening level every frame.
        while (!geometry_.empty() && geometryBytes_+bytes>256*1024*1024) {
            auto oldest=std::min_element(geometry_.begin(),geometry_.end(),[](const auto& l,const auto& r){return l.second.used<r.second.used;});
            transientBuffers_.recycle(oldest->second.buffer,oldest->second.transientCapacity,D3D11_BIND_VERTEX_BUFFER);
            geometryBytes_-=oldest->second.bytes();geometry_.erase(oldest);
        }
        geometryBytes_+=bytes;++vertexUploads_;
        if (storedUpload) frameUploadBytes_+=bytes;
        it=geometry_.emplace(g.vertices.get(),std::move(native)).first;
        noteWorstUpload(millisBetween(uploadStart,Clock::now()),bytes,it->second.transientCapacity?"vertex-transient":"vertex");
    }
    it->second.used=++resourceUse_;
    const auto& indices=g.indices->indices;
    WorldVertexShaderD3D11* shaderPtr = nullptr;
    if (lastShaderValid_ && draw.options == lastShaderOptions_) {
        shaderPtr = lastShader_;
    } else {
        const auto key=shaderKey(draw.options);auto& shader=cacheEntry(shaders_,std::string_view(key.data(),key.size()));
        if (!shader) shader=std::make_unique<WorldVertexShaderD3D11>(device_.Get(),draw.options,true);
        shaderPtr = shader.get();
        lastShaderOptions_ = draw.options; lastShader_ = shaderPtr; lastShaderValid_ = true;
    }
    timing(profileGeometry_);
    Native::setRenderSamplePhase(Native::RenderSamplePhase::constants);
    if (!shaderPtr->bind(context_.Get(),draw.constants,it->second.vertices,&it->second.indexBounds,
                      bindingsValid_ && boundVertex_==shaderPtr)) return rejected(10);
    boundVertex_=shaderPtr;
    timing(profileBinding_);
    Native::setRenderSamplePhase(Native::RenderSamplePhase::states);
    if(!viewportUploaded_ || std::memcmp(uploadedViewport_.data(),draw.depthRange.data(),sizeof(uploadedViewport_))) {
        updateConstants(context_.Get(),viewportConstants_.Get(),draw.depthRange.data(),sizeof(uploadedViewport_));
        uploadedViewport_=draw.depthRange;viewportUploaded_=true;
    }
    if(!bindingsValid_) {
        ID3D11Buffer* viewportBuffer=viewportConstants_.Get();context_->VSSetConstantBuffers(2,1,&viewportBuffer);
        pixelBuffersBound_=false;
    }
    auto cachedIndex=indices_.find(g.indices.get());
    Native::setRenderSamplePhase(Native::RenderSamplePhase::indices);
    if(cachedIndex==indices_.end()) {
        const size_t bytes=indices.size()*sizeof(uint16_t);
        if(bytes>UINT_MAX)return rejected(5);
        const bool storedIndices = (g.indices->address != 0 || g.indices->id != 0);
        if (storedIndices && !budgetFor(bytes)) return rejected(12);
        const auto uploadStart=Clock::now();
        Indices native;native.source=g.indices;native.maximum=*std::max_element(indices.begin(),indices.end());
        D3D11_BUFFER_DESC ib{};ib.ByteWidth=UINT(bytes);ib.BindFlags=D3D11_BIND_INDEX_BUFFER;ib.Usage=D3D11_USAGE_IMMUTABLE;
        if(!g.indices->address && !g.indices->id)
            native.transientCapacity=transientBuffers_.upload(indices.data(),ib.ByteWidth,ib.BindFlags,native.buffer);
        else {
            D3D11_SUBRESOURCE_DATA data{indices.data(),0,0};check(device_->CreateBuffer(&ib,&data,&native.buffer),"world IB");
        }
        const size_t allocationBytes=native.bytes();
        while(!indices_.empty() && indexBytes_+allocationBytes>64*1024*1024) {
            auto oldest=std::min_element(indices_.begin(),indices_.end(),[](const auto& l,const auto& r){return l.second.used<r.second.used;});
            transientBuffers_.recycle(oldest->second.buffer,oldest->second.transientCapacity,D3D11_BIND_INDEX_BUFFER);
            indexBytes_-=oldest->second.bytes();indices_.erase(oldest);
        }
        cachedIndex=indices_.emplace(g.indices.get(),std::move(native)).first;indexBytes_+=allocationBytes;++indexUploads_;
        if (storedIndices) frameUploadBytes_+=allocationBytes;
        noteWorstUpload(millisBetween(uploadStart,Clock::now()),allocationBytes,cachedIndex->second.transientCapacity?"index-transient":"index");
    }
    cachedIndex->second.used=++resourceUse_;
    // Most complete immutable index buffers fit this vertex stream. Keep the
    // subset fallback for shared IBs whose other draws use a larger VB.
    if(cachedIndex->second.maximum>=it->second.vertices.size())
        for(size_t i=g.firstIndex;i<g.firstIndex+count;++i)
            if(indices[i]>=it->second.vertices.size())return rejected(9);
    Native::setRenderSamplePhase(Native::RenderSamplePhase::states);
    auto width=draw.viewport[0]+draw.viewport[2],height=draw.viewport[1]+draw.viewport[3];
    // Allocation history can leave the two attachments at different extents.
    // D3D11 requires a matching pair; retain the union before obtaining views
    // so neither attachment shrinks or loses pixels from an earlier viewport.
    auto retainExtent=[&](uint64_t key) {
        if(const auto found=surfaces_.find(key);found!=surfaces_.end()) {
            width=(std::max)(width,found->second.width);height=(std::max)(height,found->second.height);
        }
    };
    if(draw.targets[4])retainExtent(draw.surfaceKey(4));
    if(draw.material!=Native::WorldMaterial::depth)retainExtent(draw.surfaceKey(0));
    auto* depth=draw.targets[4]?surface(draw.surfaceKey(4),width,height,true).depth.Get():nullptr;
    ID3D11RenderTargetView* color=draw.material==Native::WorldMaterial::depth?nullptr:surface(draw.surfaceKey(0),width,height,false).color.Get();
    if(!bindingsValid_ || boundColor_!=color || boundDepth_!=depth) {
        boundViews_.fill(nullptr);context_->PSSetShaderResources(0,16,boundViews_.data());
        context_->OMSetRenderTargets(color?1:0,color?&color:nullptr,depth);
        boundColor_=color;boundDepth_=depth;
    }
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=(flags&2)!=0;ds.DepthWriteMask=(flags&4)?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc=WorldRenderState::depthComparison(a[96]);ds.StencilEnable=(flags&0x4000)!=0;ds.StencilReadMask=a[125];ds.StencilWriteMask=a[126];
    ds.FrontFace=WorldRenderState::stencilFace(a+120);ds.BackFace=(flags&0x80)?WorldRenderState::stencilFace(a+122):ds.FrontFace;
    if(depthStates_.size()>512)depthStates_.clear();
    auto& depthState=depthStates_.entry(ds);
    if(!depthState)check(device_->CreateDepthStencilState(&ds,&depthState),"world depth state");
    if(!bindingsValid_ || boundDepthState_!=depthState.Get() || boundStencil_!=a[124]) {
        context_->OMSetDepthStencilState(depthState.Get(),a[124]);boundDepthState_=depthState.Get();boundStencil_=a[124];
    }
    D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=UINT8(((flags&0x100000)?7:0)|((flags&0x1000000)?8:0));
    blend.AlphaToCoverageEnable=(flags&1)!=0;
    if (flags&8) {
        constexpr D3D11_BLEND factors[]{D3D11_BLEND_ZERO,D3D11_BLEND_ZERO,D3D11_BLEND_ONE,D3D11_BLEND_SRC_COLOR,D3D11_BLEND_INV_SRC_COLOR,
            D3D11_BLEND_SRC_ALPHA,D3D11_BLEND_INV_SRC_ALPHA,D3D11_BLEND_DEST_ALPHA,D3D11_BLEND_INV_DEST_ALPHA,
            D3D11_BLEND_DEST_COLOR,D3D11_BLEND_INV_DEST_COLOR,D3D11_BLEND_SRC_ALPHA_SAT};
        if (a[144]>=std::size(factors) || a[145]>=std::size(factors)) return rejected(11);
        auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=factors[a[144]];rt.DestBlend=factors[a[145]];
        auto alpha=[](D3D11_BLEND f) {return f==D3D11_BLEND_SRC_COLOR?D3D11_BLEND_SRC_ALPHA:f==D3D11_BLEND_INV_SRC_COLOR?D3D11_BLEND_INV_SRC_ALPHA:
            f==D3D11_BLEND_DEST_COLOR?D3D11_BLEND_DEST_ALPHA:f==D3D11_BLEND_INV_DEST_COLOR?D3D11_BLEND_INV_DEST_ALPHA:f;};
        rt.SrcBlendAlpha=alpha(rt.SrcBlend);rt.DestBlendAlpha=alpha(rt.DestBlend);rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    }
    if(blendStates_.size()>512)blendStates_.clear();
    auto& blendState=blendStates_.entry(blend);
    if(!blendState)check(device_->CreateBlendState(&blend,&blendState),"world blend state");
    if(!bindingsValid_ || boundBlend_!=blendState.Get()) {
        context_->OMSetBlendState(blendState.Get(),nullptr,~0u);boundBlend_=blendState.Get();
    }
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=(flags&0x800)?D3D11_CULL_BACK:D3D11_CULL_NONE;
    // Original822482CC sets face=1 (CW front) when culling is enabled
    // without0x1000, and face=0 (CCW front) with0x1000. Disabling culling
    // clears both cull and face bits, also defining the stencil front side.
    raster.FrontCounterClockwise=!(flags&0x800) || (flags&0x1000)!=0;
    raster.DepthClipEnable=TRUE;raster.ScissorEnable=(flags&0x20)!=0;
    if(rasterStates_.size()>512)rasterStates_.clear();
    auto& rasterState=rasterStates_.entry(raster);
    if(!rasterState)check(device_->CreateRasterizerState(&raster,&rasterState),"world raster state");
    if(!bindingsValid_ || boundRaster_!=rasterState.Get()) {context_->RSSetState(rasterState.Get());boundRaster_=rasterState.Get();}
    const auto leftTop=word(a+112),rightBottom=word(a+116);
    D3D11_RECT rect{LONG((leftTop&65535)*scale_),LONG((leftTop>>16)*scale_),LONG((rightBottom&65535)*scale_),LONG((rightBottom>>16)*scale_)};
    if(!bindingsValid_ || std::memcmp(&boundScissor_,&rect,sizeof(rect))) {context_->RSSetScissorRects(1,&rect);boundScissor_=rect;}
    D3D11_VIEWPORT viewport{float(draw.viewport[0]*scale_),float(draw.viewport[1]*scale_),float(draw.viewport[2]*scale_),float(draw.viewport[3]*scale_),0,1};
    if(!bindingsValid_ || std::memcmp(&boundViewport_,&viewport,sizeof(viewport))) {context_->RSSetViewports(1,&viewport);boundViewport_=viewport;}
    auto* pixel=!fragFound?nullptr:frag->shader.Get();
    if(!bindingsValid_ || boundPixel_!=pixel) {context_->PSSetShader(pixel,nullptr,0);boundPixel_=pixel;}
    if(fragFound) {
        struct AlphaTest {uint32_t function;float reference;uint32_t padding[2];};
        const AlphaTest alpha{(flags&1)?8u:unsigned(a[97]),float((unsigned(a[98])<<8)|a[99])/255.0f,{0,0}};
        static_assert(sizeof(alpha)==sizeof(uploadedAlpha_));
        if(!pixelConstantsUploaded_ || std::memcmp(uploadedAlpha_.data(),&alpha,sizeof(alpha))) {
            updateConstants(context_.Get(),alphaTestConstants_.Get(),&alpha,sizeof(alpha));
            std::memcpy(uploadedAlpha_.data(),&alpha,sizeof(alpha));
        }
        if(!pixelConstantsUploaded_ || std::memcmp(uploadedFragment_.data(),draw.fragmentConstants.data(),sizeof(draw.fragmentConstants))) {
            // The engine supplies at most16 fragment vectors. The remaining
            // bank is permanently zero; don't rebuild/compare it every draw.
            std::copy(draw.fragmentConstants.begin(),draw.fragmentConstants.end(),uploadedFragment_.begin());
            updateConstants(context_.Get(),fragmentConstants_.Get(),uploadedFragment_.data(),sizeof(uploadedFragment_));
        }
        if(!pixelConstantsUploaded_ || std::memcmp(uploadedScales_.data(),scales.data(),sizeof(scales))) {
            updateConstants(context_.Get(),textureScales_.Get(),scales.data(),sizeof(scales));
            uploadedScales_=scales;
        }
        pixelConstantsUploaded_=true;
        if(!pixelBuffersBound_) {
            ID3D11Buffer* buffers[]{fragmentConstants_.Get(),textureScales_.Get(),alphaTestConstants_.Get()};
            context_->PSSetConstantBuffers(0,3,buffers);pixelBuffersBound_=true;
        }
        if(!bindingsValid_ || boundViews_!=views) {context_->PSSetShaderResources(0,16,views.data());boundViews_=views;}
        if(!bindingsValid_ || boundSamplers_!=samplers) {context_->PSSetSamplers(0,16,samplers.data());boundSamplers_=samplers;}
    }
    ID3D11Buffer* vb=it->second.buffer.Get();UINT stride=sizeof(Native::WorldVertex),offset=0;
    if(!bindingsValid_ || boundVB_!=vb) {context_->IASetVertexBuffers(0,1,&vb,&stride,&offset);boundVB_=vb;}
    auto* ib=cachedIndex->second.buffer.Get();
    if(!bindingsValid_ || boundIB_!=ib) {context_->IASetIndexBuffer(ib,DXGI_FORMAT_R16_UINT,0);boundIB_=ib;}
    if(!bindingsValid_) {
        context_->GSSetShader(nullptr,nullptr,0);context_->HSSetShader(nullptr,nullptr,0);context_->DSSetShader(nullptr,nullptr,0);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    // Task-54 bounded smoke evidence. Explicit-inspection only: the
    // inspectFrame_ gate skips all hashing/allocation/logging on normal
    // runs. Separated from acceptance: every rejection above already
    // returned, so only accepted blended draws of the two candidate smoke
    // programs are described, and nothing below mutates renderer state.
    if(inspectFrame_ && inspection_<=16 &&
        (draw.fragmentName=="XRUtil_RenderSurface" || draw.fragmentName=="XRShader_FP20_NDSEATP") &&
        (flags&8)) {
        if(smokeRecords_<128) {
            smokeEvidence(draw,flags,it->second.vertices,smokeOrdinal);
            ++smokeRecords_;
        } else if(!smokeTruncated_) {
            smokeTruncated_=true;
            std::fprintf(stderr,"[SmokeEvidenceTruncated] inspection=%u frame=%llu records=128\n",
                inspection_,presentCount_);
        }
    }
    timing(profileStates_);
    Native::setRenderSamplePhase(Native::RenderSamplePhase::submit);
    const unsigned material=unsigned(draw.material);const auto ordinal=++drawOrdinals_[material];
    const bool inspectLighting=diagnostics_ && draw.material==Native::WorldMaterial::ndsp && (ordinal==64 || ordinal==512);
    const auto beforeLighting=inspectLighting?readSurface(draw,0):std::vector<uint8_t>{};
    const bool measure=diagnostics_ && !queryPending_[material] && (ordinal==1 || ordinal==64 || ordinal==512 || ordinal==1024);
    if (measure) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS,0};
        if (!queries_[material]) check(device_->CreateQuery(&desc,&queries_[material]),"world pipeline query");
        context_->Begin(queries_[material].Get());
    }
    context_->DrawIndexed(count,g.firstIndex,0);
    bindingsValid_=true;
    if (measure) {context_->End(queries_[material].Get());queryPending_[material]=true;queryOrdinals_[material]=ordinal;}
    if (inspectLighting) {
        const auto after=readSurface(draw,0);size_t changed=0,nonfinite=0;
        if (after.size()==beforeLighting.size()) for(size_t i=0;i+8<=after.size();i+=8) {
            if (std::memcmp(beforeLighting.data()+i,after.data()+i,6)) ++changed;
            for(unsigned channel=0;channel<3;++channel) {
                uint16_t half;std::memcpy(&half,after.data()+i+channel*2,2);
                if ((half&0x7C00)==0x7C00) ++nonfinite;
            }
        }
        std::fprintf(stderr,"[EngineWorldPixels] litDraw=%llu changedRgbPixels=%zu nonfiniteRgbChannels=%zu surface=%08X\n",
            ordinal,changed,nonfinite,draw.targets[0]);
    }
    bool inspectProgram=diagnostics_ && presentCount_==60;
    if(!inspectProgram && inspectFrame_ && inspectedPrograms_.size()<64) {
        const auto inspectionKey=draw.fragmentName+":"+std::to_string(draw.fragmentFlags)+":"+std::to_string(draw.targets[0]);
        inspectProgram=inspectedPrograms_.insert(inspectionKey).second;
    }
    if(inspectProgram && draw.material!=Native::WorldMaterial::depth) {
        const auto pixels=readSurface(draw,0);size_t nonzero=0,nonfinite=0;double sum[3]{};float maximum[3]{};
        auto halfFloat=[](uint16_t h){const unsigned e=(h>>10)&31,m=h&1023;return std::ldexp(float(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
        for(size_t p=0;p+8<=pixels.size();p+=8) {
            uint16_t rgba[4]{};std::memcpy(rgba,pixels.data()+p,8);if(rgba[0] || rgba[1] || rgba[2])++nonzero;
            for(unsigned c=0;c<3;++c){if((rgba[c]&0x7c00)==0x7c00){++nonfinite;continue;}
                const auto value=halfFloat(rgba[c]);sum[c]+=value;maximum[c]=(std::max)(maximum[c],value);}
        }
        std::fprintf(stderr,"[EngineWorldPassPixels] inspection=%u frame=%llu program=%s flags=%u target=%08X nonzero=%zu nonfinite=%zu mean=%g,%g,%g max=%g,%g,%g constant0=%g,%g,%g,%g\n",
            inspection_,presentCount_,draw.fragmentName.c_str(),draw.fragmentFlags,draw.targets[0],nonzero,nonfinite,sum[0]/(pixels.size()/8),sum[1]/(pixels.size()/8),sum[2]/(pixels.size()/8),maximum[0],maximum[1],maximum[2],
            draw.fragmentConstants[0][0],draw.fragmentConstants[0][1],draw.fragmentConstants[0][2],draw.fragmentConstants[0][3]);
    }
    timing(profileSubmit_);++profileDraws_;
    Native::recordWorldSubmission(draw.material);return true;
}
// Task-54 opt-in smoke evidence. Called only for accepted blended draws of
// XRUtil_RenderSurface / XRShader_FP20_NDSEATP while inspectFrame_ is set,
// at most 128 records per inspection and only for inspections 1..16 (see
// world_renderer.h). Reads owned snapshot data and CPU-side D3D11
// descriptors only: no guest-memory dereference, no GPU readback, no cache
// mutation. StoredGeometry carries no per-upload generation counter, so
// identity is reported via addresses/ids plus sampled content hashes.
void WorldRendererD3D11::smokeEvidence(const Native::WorldDraw& draw,uint32_t flags,
    const std::vector<Native::WorldVertex>& decoded,unsigned ordinal) {
    const auto* a=draw.attributes.data();
    const uint32_t alphaFunc=a[97];
    const uint32_t alphaRefRaw=(uint32_t(a[98])<<8)|a[99];
    std::fprintf(stderr,
        "[SmokeEvidence] inspection=%u frame=%llu draw=%u program=%s flags=%u beFlags=%08X "
        "alphaFunc=%u alphaRefRaw=%u alphaRef=%g blendSrc=%u blendDst=%u depthTest=%u depthWrite=%u depthFunc=%u "
        "stencil=%u cull=%u writeRGB=%u writeA=%u targets=%08X,%08X,%08X,%08X,%08X viewport=%u,%u,%u,%u depthRange=%g,%g\n",
        inspection_,presentCount_,ordinal,
        draw.fragmentName.c_str(),draw.fragmentFlags,flags,
        alphaFunc,alphaRefRaw,double(float(alphaRefRaw)/255.0f),unsigned(a[144]),unsigned(a[145]),
        unsigned((flags&2)!=0),unsigned((flags&4)!=0),unsigned(a[96]),
        unsigned((flags&0x4000)!=0),unsigned((flags&0x800)!=0),
        unsigned((flags&0x100000)!=0),unsigned((flags&0x1000000)!=0),
        unsigned(draw.targets[0]),unsigned(draw.targets[1]),unsigned(draw.targets[2]),
        unsigned(draw.targets[3]),unsigned(draw.targets[4]),
        unsigned(draw.viewport[0]),unsigned(draw.viewport[1]),
        unsigned(draw.viewport[2]),unsigned(draw.viewport[3]),
        double(draw.depthRange[0]),double(draw.depthRange[1]));
    const auto& geometry=draw.geometry;
    const Native::StoredGeometry* stored=geometry.vertices.get();
    const Native::StoredGeometry* storedIndices=geometry.indices.get();
    if(!stored || !storedIndices) {
        std::fprintf(stderr,"[SmokeEvidenceGeometry] inspection=%u frame=%llu draw=%u missing\n",
            inspection_,presentCount_,ordinal);
        return;
    }
    std::fprintf(stderr,
        "[SmokeEvidenceGeometry] inspection=%u frame=%llu draw=%u vptr=%p vaddr=%08X vid=%u vcount=%u stride=%u "
        "iptr=%p iaddr=%08X iid=%u firstIndex=%u indexCount=%u "
        "formats=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
        "modes=%u,%u,%u,%u,%u,%u,%u,%u coords=%u,%u,%u,%u,%u,%u,%u,%u "
        "weights=%u normal=%u tangents=%u normalize=%u color=%u conversionMask=%08X\n",
        inspection_,presentCount_,ordinal,
        static_cast<const void*>(stored),stored->address,stored->id,
        stored->vertexCount,stored->stride,
        static_cast<const void*>(storedIndices),storedIndices->address,storedIndices->id,
        geometry.firstIndex,geometry.indexCount,
        unsigned(stored->formats[0]),unsigned(stored->formats[1]),unsigned(stored->formats[2]),unsigned(stored->formats[3]),
        unsigned(stored->formats[4]),unsigned(stored->formats[5]),unsigned(stored->formats[6]),unsigned(stored->formats[7]),
        unsigned(stored->formats[8]),unsigned(stored->formats[9]),unsigned(stored->formats[10]),unsigned(stored->formats[11]),
        unsigned(stored->formats[12]),unsigned(stored->formats[13]),unsigned(stored->formats[14]),unsigned(stored->formats[15]),
        unsigned(draw.options.modes[0]),unsigned(draw.options.modes[1]),unsigned(draw.options.modes[2]),unsigned(draw.options.modes[3]),
        unsigned(draw.options.modes[4]),unsigned(draw.options.modes[5]),unsigned(draw.options.modes[6]),unsigned(draw.options.modes[7]),
        unsigned(draw.options.coordinates[0]),unsigned(draw.options.coordinates[1]),unsigned(draw.options.coordinates[2]),unsigned(draw.options.coordinates[3]),
        unsigned(draw.options.coordinates[4]),unsigned(draw.options.coordinates[5]),unsigned(draw.options.coordinates[6]),unsigned(draw.options.coordinates[7]),
        draw.options.weights,unsigned(draw.options.normal),unsigned(draw.options.tangents),
        unsigned(draw.options.normalizeNormal),unsigned(draw.options.vertexColor),stored->conversionMask);
    const auto& ownedIndices=storedIndices->indices;
    const uint32_t count=geometry.indexCount,first=geometry.firstIndex;
    const unsigned sampledIndices=count<64u?count:64u;
    uint64_t indexHash=14695981039346656037ull,vertexHash=14695981039346656037ull;
    unsigned hashedVertices=0;
    const size_t rawSize=stored->vertices.size();
    const uint32_t stride=stored->stride;
    for(unsigned n=0;n<sampledIndices;++n) {
        const uint16_t indexValue=ownedIndices[size_t(first)+n];
        indexHash=smokeHashBytes(&indexValue,sizeof(indexValue),indexHash);
        if(hashedVertices<32 && stride) {
            const uint64_t offset=uint64_t(indexValue)*stride;
            if(offset+stride<=rawSize) {
                vertexHash=smokeHashBytes(stored->vertices.data()+offset,stride,vertexHash);
                ++hashedVertices;
            }
        }
    }
    std::fprintf(stderr,
        "[SmokeEvidenceSample] inspection=%u frame=%llu draw=%u sampled=first%uIndices/%uVertices "
        "indexHash=%016llX vertexHash=%016llX\n",
        inspection_,presentCount_,ordinal,sampledIndices,hashedVertices,
        static_cast<unsigned long long>(indexHash),static_cast<unsigned long long>(vertexHash));
    const unsigned showVertices=sampledIndices<3u?sampledIndices:3u;
    for(unsigned n=0;n<showVertices;++n) {
        const uint16_t indexValue=ownedIndices[size_t(first)+n];
        if(size_t(indexValue)>=decoded.size())continue;
        const auto& v=decoded[indexValue];
        std::fprintf(stderr,
            "[SmokeEvidenceVertex] inspection=%u frame=%llu draw=%u n=%u index=%u "
            "pos=%g,%g,%g,%g normal=%g,%g,%g,%g tex0=%g,%g,%g,%g color=%g,%g,%g,%g\n",
            inspection_,presentCount_,ordinal,n,unsigned(indexValue),
            double(v.position[0]),double(v.position[1]),double(v.position[2]),double(v.position[3]),
            double(v.normal[0]),double(v.normal[1]),double(v.normal[2]),double(v.normal[3]),
            double(v.tex[0][0]),double(v.tex[0][1]),double(v.tex[0][2]),double(v.tex[0][3]),
            double(v.color[0]),double(v.color[1]),double(v.color[2]),double(v.color[3]));
    }
    const uint64_t fragmentHash=smokeHashBytes(draw.fragmentConstants.data(),sizeof(draw.fragmentConstants));
    uint64_t constantHash=smokeHashBytes(draw.constants.vectors.data(),sizeof(draw.constants.vectors));
    constantHash=smokeHashBytes(draw.constants.references.data(),sizeof(draw.constants.references),constantHash);
    std::fprintf(stderr,
        "[SmokeEvidenceConstants] inspection=%u frame=%llu draw=%u fragmentHash=%016llX vertexHash=%016llX "
        "frag0=%g,%g,%g,%g frag1=%g,%g,%g,%g frag2=%g,%g,%g,%g frag3=%g,%g,%g,%g "
        "vert0=%g,%g,%g,%g vert1=%g,%g,%g,%g vert2=%g,%g,%g,%g vert3=%g,%g,%g,%g\n",
        inspection_,presentCount_,ordinal,
        static_cast<unsigned long long>(fragmentHash),static_cast<unsigned long long>(constantHash),
        double(draw.fragmentConstants[0][0]),double(draw.fragmentConstants[0][1]),
        double(draw.fragmentConstants[0][2]),double(draw.fragmentConstants[0][3]),
        double(draw.fragmentConstants[1][0]),double(draw.fragmentConstants[1][1]),
        double(draw.fragmentConstants[1][2]),double(draw.fragmentConstants[1][3]),
        double(draw.fragmentConstants[2][0]),double(draw.fragmentConstants[2][1]),
        double(draw.fragmentConstants[2][2]),double(draw.fragmentConstants[2][3]),
        double(draw.fragmentConstants[3][0]),double(draw.fragmentConstants[3][1]),
        double(draw.fragmentConstants[3][2]),double(draw.fragmentConstants[3][3]),
        double(draw.constants.vectors[0][0]),double(draw.constants.vectors[0][1]),
        double(draw.constants.vectors[0][2]),double(draw.constants.vectors[0][3]),
        double(draw.constants.vectors[1][0]),double(draw.constants.vectors[1][1]),
        double(draw.constants.vectors[1][2]),double(draw.constants.vectors[1][3]),
        double(draw.constants.vectors[2][0]),double(draw.constants.vectors[2][1]),
        double(draw.constants.vectors[2][2]),double(draw.constants.vectors[2][3]),
        double(draw.constants.vectors[3][0]),double(draw.constants.vectors[3][1]),
        double(draw.constants.vectors[3][2]),double(draw.constants.vectors[3][3]));
    for(unsigned slot=0;slot<16;++slot)if(draw.textureMask&(1u<<slot)) {
        const auto& texture=draw.textureObjects[slot];
        const auto& cpu=draw.textures[slot];
        const auto found=resolved_.find(texture.key());
        const bool hasResolved=found!=resolved_.end();
        unsigned srvFormat=0,srvDim=0,resolvedWidth=0,resolvedHeight=0;
        if(hasResolved) {
            resolvedWidth=found->second.width;resolvedHeight=found->second.height;
            if(found->second.view) {
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
                found->second.view->GetDesc(&viewDesc);
                srvFormat=unsigned(viewDesc.Format);srvDim=unsigned(viewDesc.ViewDimension);
            }
        }
        const auto& sampled=draw.samplers[slot];
        std::fprintf(stderr,
            "[SmokeEvidenceTexture] inspection=%u frame=%llu draw=%u slot=%u texId=%u object=%08X storage=%08X "
            "format=%u size=%ux%u firstMip=%u mipLevels=%u cpu=%p cpuSize=%ux%u cpuFirstMip=%u cpuMips=%u "
            "resolved=%u resolvedSize=%ux%u srvFormat=%u srvDim=%u "
            "smp=%u,%u,%u,%u,%u,%u addr=%u,%u,%u aniso=%u min=%u max=%u border=%u bias=%g\n",
            inspection_,presentCount_,ordinal,slot,unsigned(draw.textureIds[slot]),
            texture.object,texture.storage,texture.format,texture.width,texture.height,
            texture.firstMip,texture.mipLevels,
            static_cast<const void*>(cpu.get()),cpu?cpu->width:0u,cpu?cpu->height:0u,
            cpu?cpu->firstMip:0u,cpu?static_cast<unsigned>(cpu->mips.size()):0u,
            unsigned(hasResolved),resolvedWidth,resolvedHeight,srvFormat,srvDim,
            unsigned(sampled.valid),unsigned(sampled.minLinear),unsigned(sampled.magLinear),
            unsigned(sampled.mipLinear),unsigned(sampled.baseOnly),unsigned(sampled.lodValid),
            unsigned(sampled.address[0]),unsigned(sampled.address[1]),unsigned(sampled.address[2]),
            sampled.anisotropy,sampled.minLevel,sampled.maxLevel,
            sampled.border,double(sampled.bias));
    }
}
void WorldRendererD3D11::printPerformance() {
    std::fprintf(stderr,"[TransientGeometry] created=%llu reused=%llu evicted=%llu retainedEntries=%zu retainedKiB=%.2f fenceDeferred=%llu\n",
        transientBuffers_.created(),transientBuffers_.reused(),transientBuffers_.evicted(),transientBuffers_.retainedEntries(),double(transientBuffers_.retainedBytes())/1024,transientBuffers_.fenceDeferred());
    std::fprintf(stderr,"[RenderCache] vertexUploads=%llu indexUploads=%llu vertexEntries=%zu indexEntries=%zu vertexMiB=%.2f indexMiB=%.2f depthStates=%zu blendStates=%zu rasterStates=%zu\n",
                 vertexUploads_,indexUploads_,geometry_.size(),indices_.size(),double(geometryBytes_)/(1024*1024),double(indexBytes_)/(1024*1024),depthStates_.size(),blendStates_.size(),rasterStates_.size());
    if(Native::profileEngineCpu)
        std::fprintf(stderr,"[RenderCPU] draws=%llu texturesMs=%.2f geometryMs=%.2f bindingMs=%.2f statesMs=%.2f submitMs=%.2f imageUploads=%llu imageMiB=%.2f samplers=%zu\n",
                     profileDraws_,profileTextures_,profileGeometry_,profileBinding_,profileStates_,profileSubmit_,imageUploads_,double(imageBytes_)/(1024*1024),samplerStates_.size());
    else
        std::fprintf(stderr,"[RenderCPU] draws=%llu timing=disabled imageUploads=%llu imageMiB=%.2f samplers=%zu\n",
                     profileDraws_,imageUploads_,double(imageBytes_)/(1024*1024),samplerStates_.size());
    profileDraws_=0;profileTextures_=profileGeometry_=profileBinding_=profileStates_=profileSubmit_=0;
    std::fprintf(stderr,"[RenderClear] fullRegions=%llu partialRegions=%llu emptyRegions=%llu\n",fullClearRegions_,partialClearRegions_,emptyClearRegions_);
    std::fprintf(stderr,"[RenderBudget] deferred=%llu budgetMiB=%zu worstDecodeMs=%.3f worstDecodeBytes=%zu worstUploadMs=%.3f worstUploadBytes=%zu worstKind=%s\n",budgetDeferred_,kFrameUploadBudgetBytes/(1024*1024),worstDecodeMs_,worstDecodeBytes_,worstUploadMs_,worstUploadBytes_,worstUploadKind_);
    worstDecodeMs_=0;worstDecodeBytes_=0;worstUploadMs_=0;worstUploadBytes_=0;worstUploadKind_="";
}
void WorldRendererD3D11::beginFrame() {
    Native::setRenderSamplePhase(Native::RenderSamplePhase::frame);
    frameUploadBytes_ = 0;
    transientBuffers_.beginFrame();
    const auto graphics = Native::graphicsSettings();
    bloom_ = graphics.bloom;
    motionBlur_ = graphics.motionBlur;
    invalidateBindings();
    for(auto it=images_.begin();it!=images_.end();) {
        if(it->second.source.use_count()==1) {imageBytes_-=it->second.bytes;it=images_.erase(it);} else ++it;
    }
    // Immediate geometry is owned by a single frame. Do not retain thousands
    // of obsolete snapshots or force scans of a full cache for every draw.
    for(auto it=geometry_.begin();it!=geometry_.end();) {
        if(it->second.source.use_count()==1+int(indices_.contains(it->first))) {
            transientBuffers_.recycle(it->second.buffer,it->second.transientCapacity,D3D11_BIND_VERTEX_BUFFER);
            geometryBytes_-=it->second.bytes();it=geometry_.erase(it);
        } else ++it;
    }
    for(auto it=indices_.begin();it!=indices_.end();) {
        if(it->second.source.use_count()==1+int(geometry_.contains(it->first))) {
            transientBuffers_.recycle(it->second.buffer,it->second.transientCapacity,D3D11_BIND_INDEX_BUFFER);
            indexBytes_-=it->second.bytes();it=indices_.erase(it);
        } else ++it;
    }
}
ID3D11SamplerState* WorldRendererD3D11::sampler(const Native::WorldSampler& source) {
    D3D11_SAMPLER_DESC desc{};
    constexpr D3D11_TEXTURE_ADDRESS_MODE addresses[]{D3D11_TEXTURE_ADDRESS_WRAP,D3D11_TEXTURE_ADDRESS_MIRROR,
        D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_CLAMP,D3D11_TEXTURE_ADDRESS_BORDER,D3D11_TEXTURE_ADDRESS_BORDER};
    desc.AddressU=addresses[source.address[0]&7];desc.AddressV=addresses[source.address[1]&7];desc.AddressW=addresses[source.address[2]&7];
    desc.Filter=source.anisotropy>1?D3D11_FILTER_ANISOTROPIC:D3D11_FILTER((source.minLinear?16:0)|(source.magLinear?4:0)|(source.mipLinear?1:0));
    desc.MaxAnisotropy=source.anisotropy;desc.MipLODBias=source.bias;
    // Original 822569E0/82860880 preserves the residency minimum independently
    // of the mip filter. Base-only sampling must still honor that captured LOD.
    desc.MinLOD=source.minLevel;desc.MaxLOD=source.baseOnly?source.minLevel:source.maxLevel;
    desc.ComparisonFunc=D3D11_COMPARISON_NEVER;
    std::fill(std::begin(desc.BorderColor),std::end(desc.BorderColor),source.border?1.0f:0.0f);
    if(samplerStates_.size()>512)samplerStates_.clear();
    auto& state=samplerStates_.entry(desc);
    if(!state)check(device_->CreateSamplerState(&desc,&state),"world sampler");
    return state.Get();
}
}
