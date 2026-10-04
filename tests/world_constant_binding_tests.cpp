#include "renderer/d3d11/world_renderer.h"
#include <d3d11sdklayers.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
using Microsoft::WRL::ComPtr;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static void check(HRESULT value,const char* message) {require(SUCCEEDED(value),message);}
using Result=std::array<float,40>;

class Capture {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Buffer> input_,output_,staging_,constantStaging_;
public:
    std::array<WorldVertex,2> vertices{};
    Capture(ID3D11Device* device,ID3D11DeviceContext* context):device_(device),context_(context) {
        for(unsigned i=0;i<vertices.size();++i) {
            auto& vertex=vertices[i];vertex.position={.125f+float(i)*.25f,-.5f,.25f,1};vertex.color={1,.5f,.25f,1};
            for(auto& uv:vertex.tex)uv={.125f,.25f,.5f,1};
            vertex.indices={0,3,6,9};vertex.indices2={12,15,18,21};
            vertex.weights.fill(.125f);vertex.weights2.fill(.125f);
        }
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(vertices);desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        desc.Usage=D3D11_USAGE_IMMUTABLE;D3D11_SUBRESOURCE_DATA data{vertices.data(),0,0};
        check(device_->CreateBuffer(&desc,&data,&input_),"constant transition vertices");
        desc.ByteWidth=sizeof(Result)*vertices.size();desc.BindFlags=D3D11_BIND_STREAM_OUTPUT;desc.Usage=D3D11_USAGE_DEFAULT;
        check(device_->CreateBuffer(&desc,nullptr,&output_),"constant transition stream output");
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device_->CreateBuffer(&desc,nullptr,&staging_),"constant transition output readback");
        desc.ByteWidth=4096;check(device_->CreateBuffer(&desc,nullptr,&constantStaging_),"constant transition bank readback");
    }
    std::array<EngineVector,256> bank() {
        ComPtr<ID3D11Buffer> buffer;context_->VSGetConstantBuffers(0,1,&buffer);
        require(buffer!=nullptr,"constant transition bank missing");
        context_->CopyResource(constantStaging_.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(constantStaging_.Get(),0,D3D11_MAP_READ,0,&mapped),"constant transition bank map");
        std::array<EngineVector,256> result;std::memcpy(result.data(),mapped.pData,sizeof(result));
        context_->Unmap(constantStaging_.Get(),0);return result;
    }
    std::array<Result,2> draw(WorldVertexShaderD3D11& shader,const WorldVertexConstants& constants) {
        require(shader.bind(context_.Get(),constants,vertices),"constant transition binding rejected");
        std::array<D3D11_SO_DECLARATION_ENTRY,10> declaration{};declaration[0]={0,"SV_Position",0,0,4,0};
        for(unsigned stage=0;stage<8;++stage)declaration[stage+1]={0,"TEXCOORD",stage,0,4,0};
        declaration[9]={0,"COLOR",0,0,4,0};const UINT outStride=sizeof(Result);
        ComPtr<ID3D11GeometryShader> stream;
        check(device_->CreateGeometryShaderWithStreamOutput(shader.bytecode()->GetBufferPointer(),shader.bytecode()->GetBufferSize(),
            declaration.data(),UINT(declaration.size()),&outStride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&stream),"constant transition capture shader");
        context_->GSSetShader(stream.Get(),nullptr,0);context_->PSSetShader(nullptr,nullptr,0);
        UINT stride=sizeof(WorldVertex),offset=0;ID3D11Buffer* input=input_.Get();ID3D11Buffer* output=output_.Get();
        context_->IASetVertexBuffers(0,1,&input,&stride,&offset);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        context_->SOSetTargets(1,&output,&offset);context_->Draw(UINT(vertices.size()),0);context_->SOSetTargets(0,nullptr,nullptr);
        context_->CopyResource(staging_.Get(),output_.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(staging_.Get(),0,D3D11_MAP_READ,0,&mapped),"constant transition output map");
        std::array<Result,2> result;std::memcpy(result.data(),mapped.pData,sizeof(result));context_->Unmap(staging_.Get(),0);return result;
    }
};
static WorldVertexConstants fixture() {
    WorldVertexConstants constants;
    for(unsigned row=0;row<4;++row)constants.vectors[row][row]=1;
    constants.vectors[8]={0,1,.5f,1};constants.references[0]={50,20,16,0};constants.vectors[16].fill(1);
    constants.vectors[20].fill(1);constants.vectors[24].fill(1);constants.references[1]={24,40,0,0};
    constants.references[4][2]=30;constants.vectors[30]={.25f,.5f,.75f,1};
    for(unsigned row=0;row<4;++row)constants.vectors[40+row][row]=1;
    for(unsigned bone=0;bone<52;++bone)for(unsigned row=0;row<3;++row)constants.vectors[50+bone*3+row][row]=1;
    return constants;
}
static void transitions(ID3D11Device* device,ID3D11DeviceContext* context,Capture& capture,unsigned weights) {
    WorldVertexOptions options;options.modes.fill(4);options.modes[0]=0;options.modes[3]=9;
    options.positionConversion=true;options.conversions[0]=options.matrices[0]=true;options.weights=weights;
    WorldVertexShaderD3D11 shader(device,options);
    auto constants=fixture();auto output=capture.draw(shader,constants);
    auto bank=capture.bank();require(!std::memcmp(bank.data(),constants.vectors.data(),sizeof(bank)),"Initial GPU bank differs");
    const auto initialUploads=shader.constantUploadCount();
    constants.vectors[240]={.75f,.625f,.5f,1};constants.vectors[239][0]=std::bit_cast<float>(1u);
    const auto ignored=capture.draw(shader,constants);
    require(!std::memcmp(ignored.data(),output.data(),sizeof(output)),"Unused bank edits changed GPU output");
    auto current=capture.bank();require(shader.constantUploadCount()==initialUploads && !std::memcmp(current.data(),bank.data(),sizeof(bank)),
        "Unused bank edit uploaded or mutated the actual prior GPU bank");
    constants.references[0][2]=240;output=capture.draw(shader,constants);
    require(shader.constantUploadCount()==initialUploads+1,"New reference did not consume an earlier skipped row");
    current=capture.bank();require(!std::memcmp(current.data(),constants.vectors.data(),sizeof(current)),"Reference transition did not upload the complete bank");
    WorldVertexShaderD3D11 fresh(device,options);const auto expected=capture.draw(fresh,constants);
    require(!std::memcmp(output.data(),expected.data(),sizeof(output)),"Reference transition differed from a fresh full upload");
    // Restore the tested program after another program changed the context.
    context->ClearState();capture.draw(shader,constants);
    require(shader.constantUploadCount()==initialUploads+1,"External invalidation changed retained GPU contents");
    constexpr uint32_t bits[]{0x80000000u,1u,0x807FFFFFu};
    for(auto raw:bits) {
        constants.vectors[240][3]=std::bit_cast<float>(raw);capture.draw(shader,constants);
        current=capture.bank();require(std::bit_cast<uint32_t>(current[240][3])==raw,"Consumed signed-zero/subnormal bits were not uploaded exactly");
    }
    constants.references[0][2]=16;capture.draw(shader,constants);
    const auto before=shader.constantUploadCount();constants.vectors[240][0]=.125f;capture.draw(shader,constants);
    require(shader.constantUploadCount()==before,"Formerly consumed row stayed in the new read plan");
    constants.references[0][2]=240;const auto returned=capture.draw(shader,constants);
    WorldVertexShaderD3D11 freshAgain(device,options);const auto returnedExpected=capture.draw(freshAgain,constants);
    require(!std::memcmp(returned.data(),returnedExpected.data(),sizeof(returned)),"Returned reference reused stale GPU bytes");
    if(weights) {
        const auto retained=shader.constantUploadCount();const auto oldBank=capture.bank();
        std::array<std::array<float,2>,8> invalid{};invalid[0].fill(-1);
        auto changed=constants;changed.vectors[0][0]=2;
        require(!shader.bind(context,changed,{},&invalid),"Invalid palette bounds were accepted");
        current=capture.bank();require(shader.constantUploadCount()==retained && !std::memcmp(current.data(),oldBank.data(),sizeof(current)),
            "Rejected palette binding changed the uploaded bank");
    }
    std::printf("Constant GPU transitions weights=%u passed.\n",weights);
}
int main(int argc,char** argv) {
    try {
        const bool warp=argc>1 && std::string_view(argv[1])=="--warp";
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11InfoQueue> debug;
        check(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"constant GPU transition device");
        check(device.As(&debug),"constant GPU transition debug queue");Capture capture(device.Get(),context.Get());
        for(unsigned weights:{0u,4u,8u})transitions(device.Get(),context.Get(),capture,weights);
        WorldVertexOptions weighted;weighted.weights=4;weighted.modes.fill(4);
        WorldVertexShaderD3D11 unproven(device.Get(),weighted);auto unprovenConstants=fixture();
        capture.draw(unproven,unprovenConstants);const auto unprovenBefore=unproven.constantUploadCount();
        unprovenConstants.vectors[240][0]=.25f;
        require(unproven.bind(context.Get(),unprovenConstants,{}),"Unproven direct palette binding changed acceptance");
        require(unproven.constantUploadCount()==unprovenBefore+1,"Unproven direct palette binding skipped complete-bank equality");
        WorldVertexOptions unknown;unknown.modes.fill(4);unknown.modes[0]=2;
        WorldVertexShaderD3D11 fallback(device.Get(),unknown);auto constants=fixture();capture.draw(fallback,constants);
        const auto before=fallback.constantUploadCount();constants.vectors[240][0]=.75f;capture.draw(fallback,constants);
        require(fallback.constantUploadCount()==before+1,"Unknown shader option did not keep complete-bank uploads");
        for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
            SIZE_T bytes=0;check(debug->GetMessage(i,nullptr,&bytes),"debug message size");std::vector<uint8_t> storage(bytes);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(debug->GetMessage(i,message,&bytes),"debug message");
            require(message->Severity!=D3D11_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D11_MESSAGE_SEVERITY_CORRUPTION,
                "Constant GPU transition D3D validation failed");
        }
        std::puts("Constant GPU bank transitions, complete uploads, reference changes, shader/context switches and rejected bindings passed.");return 0;
    } catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
