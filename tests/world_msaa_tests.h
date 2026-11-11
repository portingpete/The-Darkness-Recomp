#pragma once
#include "renderer/d3d11/screen_fog_sample_positions.h"
#include <d3dcompiler.h>

// Compare the production lookup with D3D's independent native intrinsic for
// every sample index. Run on Windows hardware and WARP; the Wine-compatible
// compiler preflight separately checks the production shaders without this
// deliberately native-only oracle.
static void screenFogSamplePositionContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    const std::string source=std::string("#define SCREEN_FOG_MULTISAMPLE 1\n")+screenFogSamplePositionSource+R"(
Texture2DMS<float> depth : register(t0);
RWStructuredBuffer<float4> result : register(u0);
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint width,height,count;depth.GetDimensions(width,height,count);
    if(id.x<count)result[id.x]=float4(screenFogSamplePosition(count,id.x),depth.GetSamplePosition(id.x));
}
)";
    ComPtr<ID3DBlob> code,errors;
    const auto status=D3DCompile(source.data(),source.size(),"ScreenFogSamplePositionOracle",nullptr,nullptr,
        "main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS,0,&code,&errors);
    if(FAILED(status))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Sample position oracle compile failed");
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),"Sample position oracle shader");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=8*4*sizeof(float);buffer.StructureByteStride=4*sizeof(float);
    buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_UNORDERED_ACCESS;buffer.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ComPtr<ID3D11Buffer> output,staging;check(device->CreateBuffer(&buffer,nullptr,&output),"Sample position output");
    ComPtr<ID3D11UnorderedAccessView> uav;check(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav),"Sample position UAV");
    buffer.Usage=D3D11_USAGE_STAGING;buffer.BindFlags=buffer.MiscFlags=buffer.StructureByteStride=0;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateBuffer(&buffer,nullptr,&staging),"Sample position staging");
    for(unsigned samples:{2u,4u,8u}) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=1;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R32_FLOAT;desc.SampleDesc.Count=samples;desc.SampleDesc.Quality=D3D11_STANDARD_MULTISAMPLE_PATTERN;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> depth;check(device->CreateTexture2D(&desc,nullptr,&depth),"Sample position standard texture");
        ComPtr<ID3D11ShaderResourceView> view;check(device->CreateShaderResourceView(depth.Get(),nullptr,&view),"Sample position texture view");
        context->ClearState();context->CSSetShader(shader.Get(),nullptr,0);
        ID3D11ShaderResourceView* srv=view.Get();context->CSSetShaderResources(0,1,&srv);
        ID3D11UnorderedAccessView* target=uav.Get();context->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        context->Dispatch(1,1,1);context->ClearState();context->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Sample position readback");
        std::array<std::array<float,4>,8> positions{};std::memcpy(positions.data(),mapped.pData,sizeof(positions));context->Unmap(staging.Get(),0);
        for(unsigned i=0;i<samples;++i)require((positions[i][2]!=0 || positions[i][3]!=0) &&
            positions[i][0]==positions[i][2] && positions[i][1]==positions[i][3],
            "Screen fog sample position differs from D3D standard pattern");
        std::printf("[ScreenFogSamplePosition] samples=%u all positions exactly match native D3D\n",samples);
    }
}

static void worldMsaaContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    screenFogSamplePositionContract(device,context);
    context->ClearState();WorldRendererD3D11 renderer(device,context);
    auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=3;geometry->stride=16;
    geometry->formats[0]=4;geometry->vertices.resize(48);geometry->indices={0,1,2};
    const EngineVector positions[]{{-.75f,-.75f,.5f,1},{0,.75f,.5f,1},{.75f,-.75f,.5f,1}};
    for(unsigned v=0;v<3;++v)for(unsigned lane=0;lane<4;++lane)
        put(geometry->vertices.data()+v*16+lane*4,std::bit_cast<uint32_t>(positions[v][lane]));
    auto binding=fixture(0,false);binding.descriptor.flags=0x03000000;binding.descriptor.modes.fill(4);
    constantTexgen(binding,0,{0,0,1,0});constantTexgen(binding,1,{0,0,1,0});
    const auto descriptor=encodeEngineVertexDescriptor(binding.descriptor);binding.key={};
    for(unsigned i=0;i<5;++i)for(unsigned n=0;n<4;++n)
        binding.key[i+1]=(binding.key[i+1]<<8)|descriptor[i*4+n];
    for(unsigned i=0;i<16;++i)binding.constantBytes[7*16+i]=0;
    WorldClear clear;clear.targets={8101,0,0,0,8102};clear.viewport={0,0,64,64};clear.flags=49;
    WorldDraw draw;draw.geometry.vertices=draw.geometry.indices=geometry;draw.geometry.indexCount=3;
    draw.targets=clear.targets;draw.viewport=clear.viewport;draw.material=WorldMaterial::motion;
    require(prepareWorldVertexProgram(binding,draw.options,draw.constants),"MSAA vertex binding");
    put(draw.attributes.data()+92,0x01104006);draw.attributes[96]=8;draw.attributes[97]=8;
    draw.attributes[120]=0x80;draw.attributes[121]=2;
    draw.attributes[124]=127;draw.attributes[125]=draw.attributes[126]=255;
    auto half=[](uint16_t h) {const unsigned e=(h>>10)&31,m=h&1023;
        return std::ldexp(double(e?1024+m:m),int(e?e:1)-25)*(h&0x8000?-1:1);};
    std::vector<uint8_t> baseline,previousCoverage;
    unsigned previousSamples=0;
    for(unsigned requested:{1u,2u,4u,8u}) {
        renderer.setSampleCount(requested);
        require(renderer.sampleCount()<=requested && renderer.sampleCount()>=1,"MSAA unsupported-count fallback");
        renderer.clear(clear);require(renderer.draw(draw),"MSAA color/depth triangle");
        const auto pixels=renderer.readSurface(8101,false),depth=renderer.readSurface(8102,true);
        require(pixels.size()==64*64*8 && depth.size()==64*64*4,"MSAA readback dimensions");
        size_t partial=0,different=0;
        std::vector<uint16_t> edgeLevels;
        for(size_t at=0;at<pixels.size();at+=8) {uint16_t channels[4]{};std::memcpy(channels,pixels.data()+at,8);
            require(channels[0]==channels[1] && channels[2]==0,"MSAA introduced a color tint");
            const auto red=half(channels[0]);require(red>=0 && red<=.5,"MSAA color resolve overshoot");
            partial+=red>0 && red<.5;
            if(red>0 && red<.5 && std::find(edgeLevels.begin(),edgeLevels.end(),channels[0])==edgeLevels.end())
                edgeLevels.push_back(channels[0]);
            if(previousCoverage.size()==pixels.size())different+=bool(std::memcmp(pixels.data()+at,previousCoverage.data()+at,8));
        }
        if(requested==1) {baseline=pixels;require(partial==0,"Off contains synthetic coverage");}
        else if(renderer.sampleCount()>1)require(partial>16 && pixels!=baseline,"MSAA did not create subpixel geometry coverage");
        if(previousSamples>=2 && renderer.sampleCount()>previousSamples)
            require(different>16,"Higher MSAA sample counts collapsed to identical edge coverage");
        std::printf("[MSAAComparison] requested=%u active=%u partialPixels=%zu edgeLevels=%zu differentFrom%ux=%zu\n",
                    requested,renderer.sampleCount(),partial,edgeLevels.size(),previousSamples,different);
        previousCoverage=pixels;previousSamples=renderer.sampleCount();
        uint32_t center=0;std::memcpy(&center,depth.data()+(32*64+32)*4,4);
        require((center>>24)==127 && std::abs(double(center&0xffffff)-8388608)<=1,"MSAA lost depth/stencil");

        // Sample-count changes preserve retained color and packed depth/stencil.
        renderer.setSampleCount(1);
        require(renderer.readSurface(8101,false)==pixels && renderer.readSurface(8102,true)==depth,
                "Turning MSAA off discarded retained color or depth/stencil");
        renderer.setSampleCount(requested);
        require(renderer.readSurface(8101,false)==pixels && renderer.readSurface(8102,true)==depth,
                "Turning MSAA on discarded retained color or depth/stencil");

        renderer.setRenderScale(2);
        const auto scaled=renderer.readSurface(8101,false);
        require(scaled.size()==128*128*8,"MSAA live resolution extent");
        for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x)
            require(!std::memcmp(scaled.data()+(y*128+x)*8,pixels.data()+((y/2)*64+x/2)*8,8),
                    "MSAA live resize changed retained color");
        renderer.setRenderScale(1);
        require(renderer.readSurface(8102,true)==depth,"MSAA live resize changed packed depth/stencil");

        renderer.clear(clear);
        auto queryDraw=draw;put(queryDraw.attributes.data()+92,0x01100002);queryDraw.attributes[96]=8;
        auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();
        renderer.histogram(query,true);require(renderer.draw(queryDraw),"MSAA histogram geometry");
        renderer.readSurface(8102,true); // Helper transfers must not count as geometry.
        renderer.histogram(query,false);context->Flush();const auto deadline=GetTickCount64()+5000;
        while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {renderer.pollHistograms();Sleep(1);}
        require(query->result->samples.load()==1152,"MSAA altered logical exposure samples");
        retainedDepthGrowth(renderer,8200+requested);
        retainedAttachmentPair(renderer,context,draw,8300+requested*10);
        if(renderer.sampleCount()>1) {
            // Keep these identities separate from every retained fixture above.
            // A first partial color clear pauses the histogram; growing its
            // later depth attachment must not pause/resume that query again.
            const uint32_t colorId=0xF6001000u+requested*2,depthId=colorId+1;
            WorldClear seedColor;seedColor.targets[0]=colorId;
            seedColor.viewport={0,0,64,64};seedColor.flags=1;renderer.clear(seedColor);
            WorldClear seedDepth;seedDepth.targets[4]=depthId;
            seedDepth.viewport={0,0,32,32};seedDepth.flags=48;
            seedDepth.depth=.75f;seedDepth.stencil=0xA5;renderer.clear(seedDepth);
            auto nativeTriangle=draw;nativeTriangle.targets={colorId,0,0,0,0};
            // Leave depth unbound so the first triangle cannot grow it before
            // the combined partial clear reaches the nested helper path.
            put(nativeTriangle.attributes.data()+92,0x01100000);
            auto nestedQuery=std::make_shared<WorldQuery>();
            nestedQuery->result=std::make_shared<WorldQueryResult>();
            renderer.histogram(nestedQuery,true);
            require(renderer.draw(nativeTriangle),"MSAA pre-clear histogram triangle");
            WorldClear mixed; mixed.targets={colorId,0,0,0,depthId};
            mixed.viewport={0,0,64,64};mixed.rectangle=std::array<int32_t,4>{4,4,16,16};
            mixed.flags=49;mixed.color={1,0,0,1};mixed.depth=.25f;mixed.stencil=0x5A;
            renderer.clear(mixed);
            require(renderer.draw(nativeTriangle),"MSAA post-clear histogram triangle");
            renderer.histogram(nestedQuery,false);context->Flush();
            const auto nestedDeadline=GetTickCount64()+5000;
            while(nestedQuery->result->samples.load()==UINT64_MAX && GetTickCount64()<nestedDeadline) {
                renderer.pollHistograms();Sleep(1);
            }
            require(nestedQuery->result->samples.load()==2*1152,
                    "MSAA mixed partial clear/growth corrupted the histogram or counted helper geometry");
            ComPtr<ID3D11RenderTargetView> restoredColor;ComPtr<ID3D11DepthStencilView> restoredDepth;
            context->OMGetRenderTargets(1,&restoredColor,&restoredDepth);
            require(restoredColor && !restoredDepth,"MSAA post-clear draw lost its color-only target state");
            const auto restoredPixels=renderer.readSurface(colorId,false);
            const auto grownDepth=renderer.readSurface(depthId,true);
            require(restoredPixels.size()==64*64*8 && grownDepth.size()==64*64*4,
                    "MSAA mixed clear did not retain color extent and grow depth");
            uint16_t trianglePixel[4]{},clearPixel[4]{};
            std::memcpy(trianglePixel,restoredPixels.data()+(32*64+32)*8,8);
            std::memcpy(clearPixel,restoredPixels.data()+(8*64+8)*8,8);
            require(trianglePixel[0]==0x3800 && trianglePixel[1]==0x3800 && trianglePixel[2]==0,
                    "MSAA post-clear draw lost its native shader or constants");
            require(clearPixel[0]==0x3C00 && clearPixel[1]==0 && clearPixel[2]==0 && clearPixel[3]==0x3C00,
                    "MSAA post-clear draw damaged the partial color clear");
            uint32_t clearedDepth=0,retainedDepth=0,borderDepth=0;
            std::memcpy(&clearedDepth,grownDepth.data()+(8*64+8)*4,4);
            std::memcpy(&retainedDepth,grownDepth.data()+(20*64+20)*4,4);
            std::memcpy(&borderDepth,grownDepth.data()+(48*64+48)*4,4);
            require((clearedDepth>>24)==0x5A && std::abs(double(clearedDepth&0xFFFFFF)-4194304)<=1 &&
                    (retainedDepth>>24)==0xA5 && std::abs(double(retainedDepth&0xFFFFFF)-12582911)<=1 && borderDepth==0,
                    "MSAA mixed partial clear/growth changed retained depth/stencil or its new border");
            std::printf("[MSAANestedClear] requested=%u active=%u logicalSamples=%llu retainedDepth=64x64\n",
                requested,renderer.sampleCount(),static_cast<unsigned long long>(nestedQuery->result->samples.load()));
        }
    }
    context->ClearState();
    std::puts("WorldMSAA passed: 2x/4x/8x coverage, depth/stencil, live mode/scale retention, atlas growth and normalized exposure queries including mixed partial clear/growth.");
}
