#include "renderer/d3d11/shader_bytecode_cache.h"
#include "renderer/d3d11/world_renderer.h"
#include <d3d11shader.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

using namespace DarkRecomp;
using Microsoft::WRL::ComPtr;
namespace Bytecode=DarkRecomp::ShaderBytecodeCache;
static void require(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
static void check(HRESULT result,const char* message) {if(FAILED(result))throw std::runtime_error(message);}
static void contract(ID3D11Device* device) {
    constexpr UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_IEEE_STRICTNESS;
    constexpr std::string_view source=R"(
Texture2D<float4> image : register(t3);
SamplerState imageSampler : register(s3);
cbuffer Environment : register(b0) {float4 color;};
float4 pixelMain(float4 position:SV_Position):SV_Target {
    return image.SampleLevel(imageSampler,position.xy*0.125,0)*color;
})";
    const auto identity=Bytecode::compilerIdentity();require(bool(identity),"Imported compiler identity unavailable on test machine");
    const auto key=Bytecode::key(source,"cache_contract","pixelMain","ps_5_0",flags,0);
    require(key && *key==Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,0),"Compiler-aware shader identity changed");
    require(*key!=Bytecode::contentKey(*identity+1,source,"cache_contract","pixelMain","ps_5_0",flags,0),"Compiler change reused stale shader identity");
    require(*key!=Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags+1,0),"Compile flags reused stale shader identity");
    require(*key!=Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,1),"Secondary flags reused stale shader identity");
    require(*key!=Bytecode::contentKey(*identity,source,"cache_contract","vertexMain","ps_5_0",flags,0),"Entry point reused stale shader identity");
    require(*key!=Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","vs_5_0",flags,0),"Shader model reused stale shader identity");
    require(*key!=Bytecode::contentKey(*identity,source,"different_name","pixelMain","ps_5_0",flags,0),"Source identity reused stale shader bytes");
    require(*key!=Bytecode::contentKey(*identity,std::string(source)+"\n","cache_contract","pixelMain","ps_5_0",flags,0),"Source edit reused stale shader identity");
    const D3D_SHADER_MACRO macros1[]{{"a","bc"},{nullptr,nullptr}},macros2[]{{"ab","c"},{nullptr,nullptr}},macros3[]{{"a","bd"},{nullptr,nullptr}};
    const auto macrosKey=Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,0,macros1);
    require(macrosKey!=*key && macrosKey!=Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,0,macros2) &&
            macrosKey!=Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,0,macros3),"Macro boundaries/values reused stale shader identity");
    const D3D_SHADER_MACRO nullDefinition[]{{"a",nullptr},{nullptr,nullptr}},emptyDefinition[]{{"a",""},{nullptr,nullptr}};
    require(Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,0,nullDefinition)!=
            Bytecode::contentKey(*identity,source,"cache_contract","pixelMain","ps_5_0",flags,0,emptyDefinition),
            "Null and empty macro definitions shared an identity");
    ComPtr<ID3DBlob> code,errors,loaded;
    check(D3DCompile(source.data(),source.size(),"cache_contract",nullptr,nullptr,"pixelMain","ps_5_0",flags,0,&code,&errors),"Compile cache fixture");
    const auto directory=std::filesystem::temp_directory_path()/
        (L"DarkRecomp-shader-cache-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    Bytecode::Cache cache(directory);
    require(!cache.load(*key,loaded) && !loaded,"Cold cache returned an uninitialized shader");
    require(cache.save(*key,code.Get()) && cache.load(*key,loaded),"Cached shader did not survive disk save/load");
    require(loaded->GetBufferSize()==code->GetBufferSize() && std::memcmp(loaded->GetBufferPointer(),code->GetBufferPointer(),code->GetBufferSize())==0,
            "Cached shader bytes differ from compilation");
    ComPtr<ID3D11PixelShader> pixel;
    check(device->CreatePixelShader(loaded->GetBufferPointer(),loaded->GetBufferSize(),nullptr,&pixel),"Driver rejected valid cached shader");
    ComPtr<ID3D11ShaderReflection> reflection;
    check(D3DReflect(loaded->GetBufferPointer(),loaded->GetBufferSize(),IID_PPV_ARGS(&reflection)),"Cached shader reflection failed");
    D3D11_SHADER_INPUT_BIND_DESC texture{};
    check(reflection->GetResourceBindingDescByName("image",&texture),"Cached shader lost reflected resource");
    require(texture.Type==D3D_SIT_TEXTURE && texture.BindPoint==3 && texture.Dimension==D3D_SRV_DIMENSION_TEXTURE2D,
            "Cached shader changed reflected texture bindings");
    const auto* retained=loaded.Get();
    auto rejected=[&] {require(!cache.load(*key,loaded) && loaded.Get()==retained,"Invalid cache entry changed selected shader ownership");};
    // Mutate each header field, then repair the same path atomically.
    for(unsigned byte:{0u,8u,12u,16u,24u,32u}) {
        {
            std::fstream file(cache.path(*key),std::ios::in|std::ios::out|std::ios::binary);
            file.seekg(byte);char value=0;file.get(value);value^=0x5A;file.seekp(byte);file.put(value);
        }
        rejected();require(cache.save(*key,code.Get()),"Corrupt cache replacement failed");
        ComPtr<ID3DBlob> restored;require(cache.load(*key,restored),"Repaired cache did not load");
    }
    const auto originalSize=std::filesystem::file_size(cache.path(*key));
    std::filesystem::resize_file(cache.path(*key),originalSize-1);rejected();
    require(cache.save(*key,code.Get()),"Truncated cache replacement failed");
    std::filesystem::resize_file(cache.path(*key),originalSize+1);rejected();
    require(cache.save(*key,code.Get()),"Oversized cache replacement failed");
    // A stale, otherwise intact record copied under another key is rejected.
    std::filesystem::copy_file(cache.path(*key),cache.path(*key+1));
    require(!cache.load(*key+1,loaded) && loaded.Get()==retained,"Copied stale cache record was accepted");
    Bytecode::Cache unavailable(directory/"not-a-directory");
    {std::ofstream file(directory/"not-a-directory");file<<"fixture";}
    require(!unavailable.save(*key,code.Get()) && !unavailable.load(*key,loaded) && loaded.Get()==retained,
            "Unavailable cache path changed a compiled shader");
    // Force an atomic rename failure by locking only this synthetic target.
    const auto locked=CreateFileW(cache.path(*key).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(locked!=INVALID_HANDLE_VALUE,"Lock cache target fixture");
    const bool replacement=cache.save(*key,code.Get());CloseHandle(locked);
    require(!replacement,"Cache replacement ignored an exclusive target lock");
    for(const auto& entry:std::filesystem::directory_iterator(directory))
        require(entry.path().extension()!=L".tmp","Failed replacement left a staging file");
    ComPtr<ID3D11VertexShader> wrongStage;
    require(FAILED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&wrongStage)),
            "Device validation accepted a cached pixel shader as a vertex shader");
    std::printf("ShaderBytecodeCache passed: cold/warm byte identity, compiler/source/flags/macros identity, driver/reflection validation, corrupt/stale/truncated/oversized records, same-path recovery, unavailable directory; fixtures=%ls\n",directory.c_str());
}
int main(int argc,char** argv) {
    try {
        bool warp=false,benchmark=false;
        for(int i=1;i<argc;++i) {
            const std::string_view argument(argv[i]);
            if(argument=="--warp")warp=true;
            else if(argument=="--benchmark")benchmark=true;
            else throw std::runtime_error("Expected optional --warp and/or --benchmark");
        }
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
        check(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"Create cache-contract device");
        contract(device.Get());
        if(benchmark)for(unsigned trial=0;trial<3;++trial) {
            const auto begin=std::chrono::steady_clock::now();
            {WorldRendererD3D11 renderer(device.Get(),context.Get());}
            const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            std::printf("RendererShaderInit driver=%s trial=%u totalMs=%.3f\n",warp?"WARP":"hardware",trial+1,elapsed);
        }
        context->ClearState();return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"ShaderBytecodeCache failed: %s\n",error.what());return 1;}
}
