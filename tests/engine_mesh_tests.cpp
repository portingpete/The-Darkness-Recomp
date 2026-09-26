#include "renderer/engine/simple_mesh.h"
#include "renderer/engine/texture_upload.h"
#include "renderer/engine/texture_mip_layout.h"
#include "renderer/engine/render_trace.h"
#include "renderer/engine/decoded_geometry.h"
#include "renderer/engine/world_mesh.h"
#include "renderer/d3d11/engine_preview.h"
#include "renderer/d3d11/display_context_d3d11.h"
#include "runtime/native/runtime.h"
#include "ppc_recomp_shared.h"
#include <bit>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
extern "C" PPC_FUNC(__imp__sub_8225F320);
extern "C" PPC_FUNC(__imp__sub_82864F20);
static void require(bool result, const char* reason) { if (!result) throw std::runtime_error(reason); }
static void testDecodedDrawRequest() {
    DecodedDrawRequest request;
    constexpr uint32_t caller = 0x8225E200;
    auto consume = [&](uint32_t lr=0x8225E200, uint32_t primitive=4, uint32_t baseVertex=0,
                       uint32_t firstIndex=0, uint32_t count=6) {
        return request.consume(lr,primitive,baseVertex,firstIndex,count);
    };
    request.record(0x10000,8192,6,0);
    require(consume()==0x10000 && consume()==0,"First decoded chunk was lost or reused");
    request.record(0x20000,8192,3,24);
    require(consume(caller,4,0,12,3)==0x20000 && consume(caller,4,0,12,3)==0,
            "Second decoded chunk lost its index offset or was reused");

    // Each incorrect draw consumes the pending chunk. No later matching draw
    // may accidentally submit indices from a different original draw.
    auto wrong = [&](uint32_t lr,uint32_t primitive,uint32_t baseVertex,uint32_t firstIndex,uint32_t count) {
        request.record(0x30000,8192,6,12);
        require(!consume(lr,primitive,baseVertex,firstIndex,count),"Mismatched decoder draw accepted");
        require(!consume(caller,4,0,6,6),"Mismatched decoder draw left stale chunk");
    };
    wrong(0x8225DD38,4,0,6,6);
    wrong(caller,3,0,6,6);
    wrong(caller,4,1,6,6);
    wrong(caller,4,0,5,6);
    wrong(caller,4,0,6,3);

    auto invalid = [&](uint32_t indices,uint32_t capacity,uint32_t produced,uint32_t byteOffset) {
        request.record(0x40000,8192,6,0);
        request.record(indices,capacity,produced,byteOffset);
        require(!consume(),"Invalid or empty decoder chunk retained stale indices");
    };
    invalid(0,8192,6,0);
    invalid(0x10000,0,6,0);
    invalid(0x10000,8193,6,0);
    invalid(0x10000,8192,0,0);
    invalid(0x10000,3,6,0);
    invalid(0x10000,8192,4,0);
    invalid(0x10000,8192,6,1);
    invalid(0xFFFFFFFE,8192,3,0);
    request.record(0xFFFFFFFA,3,3,0);
    require(consume(caller,4,0,0,3)==0xFFFFFFFA,
            "Decoded index span ending at the guest address limit was rejected");
}
static void put16(uint8_t* base, uint32_t address, uint16_t v) { base[address]=uint8_t(v>>8); base[address+1]=uint8_t(v); }
static void put32(uint8_t* base, uint32_t address, uint32_t v) {
    base[address]=uint8_t(v>>24); base[address+1]=uint8_t(v>>16); base[address+2]=uint8_t(v>>8); base[address+3]=uint8_t(v);
}
static void putFloat(uint8_t* base, uint32_t address, float f) { put32(base,address,std::bit_cast<uint32_t>(f)); }
static void nearByte(uint32_t pixel, unsigned channel, int expected) {
    require(std::abs(int((pixel>>(channel*8))&255)-expected)<=1, "Native triangle pixel does not match alpha/color projection contract");
}
#include "preview_antialiasing_tests.h"
#include "preview_clear_state_tests.h"
#include "preview_video_upload_tests.h"
#include "prompt_icon_tests.h"
#include "preview_output_tests.h"
// Synthetic source provider called by the actual original 82256008 refresh.
// It updates the selected allocation and retains the temporary view so cleanup
// never enters the unrelated original heap in this isolated fixture.
static void refreshSource(PPCContext& ctx,uint8_t* base) {
    auto read=[&](uint32_t at){return uint32_t(base[at])<<24|uint32_t(base[at+1])<<16|uint32_t(base[at+2])<<8|base[at+3];};
    const auto source=ctx.r3.u32,request=ctx.r5.u32;
    const auto image=read(read(request)),view=read(image+104);
    require(ctx.r4.u32==7 && read(request+16)==0 && read(request+20)==0,
            "Original direct refresh changed source index, mip or storage-reuse flag");
    require(view==read(source+12) && read(view+24)==read(source+4),
            "Original direct refresh selected another resource view");
    put32(base,view+4,read(view+4)+1);
    std::memset(base+read(source+8),177,65536);
    put32(base,source+16,read(source+16)+1);put32(base,request+24,0x13579bdf);
}


static void putLE32(uint8_t* base, uint32_t address, uint32_t value) {
    for (unsigned i=0;i<4;++i) base[address+i]=uint8_t(value>>(8*i));
}
static constexpr uint32_t colorHeader=0x20001, colorPixels=0x21001;
static void blockFixture(uint8_t* base, uint32_t codec, uint32_t width, uint32_t height) {
    const uint32_t size=((width+3)/4)*((height+3)/4)*(codec==0?8:16);
    std::memset(base+colorHeader,0,48); std::memset(base+colorPixels,0,size+16);
    put32(base,colorHeader,0x82097610); put32(base,colorHeader+8,colorPixels); put32(base,colorHeader+12,size+16);
    put32(base,colorHeader+16,width); put32(base,colorHeader+20,height); put32(base,colorHeader+28,4);
    put32(base,colorHeader+32,0x800); put32(base,colorHeader+40,0x5014);
    putLE32(base,colorPixels,codec); putLE32(base,colorPixels+4,size); putLE32(base,colorPixels+12,16);
}
static void alphaIndices(uint8_t* base, uint32_t address) {
    uint64_t indices=0;
    for(unsigned i=0;i<16;++i) indices|=uint64_t(i%8)<<(3*i);
    for(unsigned i=0;i<6;++i) base[address+i]=uint8_t(indices>>(8*i));
}
static void testColorDecode(uint8_t* base) {
    blockFixture(base,0,5,3);
    putLE32(base,colorPixels+16,0x07E0F800); // LE RGB565 red, green.
    putLE32(base,colorPixels+20,0xE4E4E4E4); // Every row indexes 0,1,2,3.
    putLE32(base,colorPixels+24,0x0000001F); // Blue edge block.
    ColorImage image;
    require(!decodeColorImage(base,colorHeader,image) && image.width==5 && image.height==3 && image.pixels.size()==60,
            "Compressed dimensions or edge cropping changed");
    const std::vector<uint8_t> row={255,0,0,255, 0,255,0,255, 170,85,0,255, 85,170,0,255, 0,0,255,255};
    for(unsigned y=0;y<3;++y) require(std::equal(row.begin(),row.end(),image.pixels.begin()+y*20),"BC1 color/index endian mismatch");
    const auto previous=image.pixels;
    putLE32(base,colorPixels+4,15);
    require(decodeColorImage(base,colorHeader,image) && image.pixels==previous,"Malformed block length changed output");
    putLE32(base,colorPixels+4,16); putLE32(base,colorPixels+12,12);
    require(decodeColorImage(base,colorHeader,image),"Accepted overlapping compressed header");
    putLE32(base,colorPixels+12,16); put32(base,colorHeader+12,31);
    require(decodeColorImage(base,colorHeader,image),"Accepted truncated compressed allocation");
    put32(base,colorHeader+12,32); put32(base,colorHeader+8,0xFFFFFFF8);
    require(decodeColorImage(base,colorHeader,image),"Accepted wrapped compressed pointer");
    put32(base,colorHeader+8,colorPixels); put32(base,colorHeader+40,0x11014);
    require(decodeColorImage(base,colorHeader,image),"Accepted unreconstructed alternate compressed layout");
    put32(base,colorHeader+40,0x5014);
    for(uint32_t codec:{1u,2u,3u,5u}) {
        putLE32(base,colorPixels,codec);
        require(decodeColorImage(base,colorHeader,image),"Accepted unobserved block codec");
    }
    blockFixture(base,0,1,1); putLE32(base,colorPixels+16,0xFFFF0000); putLE32(base,colorPixels+20,0xFFFFFFFF);
    require(!decodeColorImage(base,colorHeader,image) && image.pixels==std::vector<uint8_t>({0,0,0,0}),"BC1 transparent index failed");
    blockFixture(base,4,4,4);
    base[colorPixels+16]=255; base[colorPixels+17]=0; alphaIndices(base,colorPixels+18);
    putLE32(base,colorPixels+24,0xF8000000); putLE32(base,colorPixels+28,0xFFFFFFFF);
    require(!decodeColorImage(base,colorHeader,image),"BC3 image rejected");
    const uint8_t alpha[8]={255,0,218,182,145,109,72,36};
    for(unsigned i=0;i<16;++i)
        require(image.pixels[i*4]==170 && image.pixels[i*4+1]==0 && image.pixels[i*4+2]==0 && image.pixels[i*4+3]==alpha[i%8],
                "BC3 must use four colors regardless of endpoint order, and unpack 48 alpha index bits");
    base[colorPixels+16]=0; base[colorPixels+17]=255;
    require(!decodeColorImage(base,colorHeader,image),"BC3 alternate alpha mode rejected");
    const uint8_t alternate[8]={0,255,51,102,153,204,0,255};
    for(unsigned i=0;i<16;++i) require(image.pixels[i*4+3]==alternate[i%8],"BC3 alternate alpha interpolation failed");
    DWORD old=0,ignored=0;
    require(VirtualProtect(base+0x21000,4096,PAGE_NOACCESS,&old),"Cannot protect compressed test data");
    require(decodeColorImage(base,colorHeader,image),"Accepted inaccessible compressed data");
    VirtualProtect(base+0x21000,4096,old,&ignored);
}

// Compare the CPU-owned transfer with independently decoded D3D11 BC texels.
// The same native color shader samples either RGBA8 or hardware BC1/BC3 at
// texel centers; opaque blending exposes alpha as well as color in readback.
// D3D11.3 sections 19.5.2/19.5.6/19.5.8 permit BC interpolation error of
// 1/255 + .03 * the larger original/promoted endpoint separation. Zero and
// one must be exact. Allow half an output UNORM8 step for target rounding.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#BCErrorTolerance
static void testColorGpu(uint8_t* base, EnginePreviewD3D11& renderer, CDisplayContextD3D11& display, SimpleMesh mesh) {
    using Microsoft::WRL::ComPtr;
    unsigned comparisons=0, maximumDifference=0;
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(display.GetDevice()->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC adapterDesc{};
        if (SUCCEEDED(adapter->GetDesc(&adapterDesc)))
            std::printf("BC reference adapter: %ls, vendor=%04X device=%04X\n",adapterDesc.Description,adapterDesc.VendorId,adapterDesc.DeviceId);
    }
    mesh.texture.reset(); mesh.video.reset(); mesh.opaque=true;
    for(auto& vertex:mesh.vertices) std::fill(std::begin(vertex.color),std::end(vertex.color),1.0f);
    for(unsigned mode=0;mode<4;++mode) {
        const bool bc3=mode>=2;
        blockFixture(base,bc3?4:0,4,4);
        const uint32_t color=colorPixels+(bc3?24:16);
        putLE32(base,color,mode==0?0x07E0F800:0xF8000000); putLE32(base,color+4,0xE4E4E4E4);
        if(bc3) {
            base[colorPixels+16]=mode==2?255:0; base[colorPixels+17]=mode==2?0:255;
            alphaIndices(base,colorPixels+18);
        }
        // These fixtures use only 0/1 endpoints, so their original and
        // promoted separations are identical. This is not a global epsilon.
        const unsigned endpointSpan[4]={255,mode==0?255u:0u,0,bc3?255u:0u};
        auto decoded=std::make_shared<ColorImage>();
        require(!decodeColorImage(base,colorHeader,*decoded),"GPU fixture decode failed"); mesh.colorTexture=decoded;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=desc.Height=4; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=bc3?DXGI_FORMAT_BC3_UNORM:DXGI_FORMAT_BC1_UNORM;
        desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{base+colorPixels+16,bc3?16u:8u,0};
        ComPtr<ID3D11Texture2D> reference; ComPtr<ID3D11ShaderResourceView> view;
        require(SUCCEEDED(display.GetDevice()->CreateTexture2D(&desc,&data,&reference)),"Hardware BC reference creation failed");
        require(SUCCEEDED(display.GetDevice()->CreateShaderResourceView(reference.Get(),nullptr,&view)),"Hardware BC reference view failed");
        for(unsigned i=0;i<16;++i) {
            for(auto& vertex:mesh.vertices) { vertex.uv[0]=(float(i%4)+0.5f)/4; vertex.uv[1]=(float(i/4)+0.5f)/4; }
            renderer.render({mesh}); const uint32_t decodedPixel=renderer.readPixel(32,32);
            ID3D11ShaderResourceView* resource=view.Get(); display.GetContext()->PSSetShaderResources(2,1,&resource);
            display.GetContext()->DrawIndexed(UINT(mesh.indices.size()),0,0);
            const uint32_t hardwarePixel=renderer.readPixel(32,32);
            for(unsigned channel=0;channel<4;++channel) {
                nearByte(decodedPixel,channel,decoded->pixels[i*4+channel]);
                const int expected=decoded->pixels[i*4+channel], actual=int((hardwarePixel>>(channel*8))&255);
                const unsigned difference=unsigned(std::abs(actual-expected));
                maximumDifference=std::max(maximumDifference,difference);
                const bool matches=(expected==0 || expected==255) ? actual==expected :
                    difference < 1.0 + 0.03*endpointSpan[channel] + 0.5;
                if (!matches) {
                    std::fprintf(stderr,"BC comparison mode=%u texel=%u channel=%u decoded=%08X hardware=%08X span=%u\n",
                                 mode,i,channel,decodedPixel,hardwarePixel,endpointSpan[channel]);
                    throw std::runtime_error("Owned BC decode exceeds D3D11 hardware tolerance");
                }
            }
            ++comparisons;
        }
    }
    std::printf("Hardware BC comparison: %u texels within D3D11 tolerance; largest UNORM8 difference=%u.\n",
                comparisons,maximumDifference);
    blockFixture(base,4,1,1); base[colorPixels+16]=128; putLE32(base,colorPixels+24,0x0000F800);
    auto translucent=std::make_shared<ColorImage>(); require(!decodeColorImage(base,colorHeader,*translucent),"1x1 BC3 rejected");
    mesh.colorTexture=translucent; mesh.opaque=false;
    for(auto& vertex:mesh.vertices) vertex.color[0]=0.5f;
    renderer.render({mesh}); auto pixel=renderer.readPixel(32,32);
    nearByte(pixel,0,64); nearByte(pixel,1,0); nearByte(pixel,2,0); nearByte(pixel,3,255);
    mesh.opaque=true; renderer.render({mesh}); pixel=renderer.readPixel(32,32);
    nearByte(pixel,0,128); nearByte(pixel,3,128);
}

// The release guard must outlive the consumer but be destroyed before the
// future joins: an assertion, allocation failure or producer exception must
// never strand a worker waiting for a part that will no longer be consumed.
template<class Produce,class Consume>
static void withLivePreviewProducer(Produce produce,Consume consume) {
    std::future<void> producer;
    struct ReleaseBackpressure {
        ~ReleaseBackpressure() {setPreviewFrameBackpressure(false);}
    } release;
    setPreviewFrameBackpressure(true,true);
    producer=std::async(std::launch::async,std::move(produce));
    consume(producer);
    require(producer.wait_for(std::chrono::seconds(5))==std::future_status::ready,
            "Live preview producer did not finish after its final part was consumed");
    producer.get();
}

static void takeLivePreviewPart(std::vector<SimpleMesh>& commands,size_t count,bool first,bool last) {
    PreviewFramePart part{!first,!last};
    require(takePreviewFrame(commands,5000,&part),"Timed out waiting for a live preview part");
    if(commands.size()!=count || part.first!=first || part.last!=last)
        std::fprintf(stderr,"Live preview part: actual commands=%zu first=%d last=%d; expected commands=%zu first=%d last=%d\n",
                     commands.size(),int(part.first),int(part.last),count,int(first),int(last));
    require(commands.size()==count && commands.size()<=512,
            "Live preview part lost commands, exceeded its bound or ended at the wrong boundary");
    require(part.first==first && part.last==last,"Live preview part changed the logical frame boundary");
}

static void requireNoPreviewPart(std::vector<SimpleMesh>& commands) {
    const auto* data=commands.data();const auto count=commands.size();
    PreviewFramePart part{false,true};
    require(!takePreviewFrame(commands,1,&part) && commands.data()==data && commands.size()==count &&
            !part.first && part.last,"Consumed live part was delivered twice or an unavailable part changed output");
}

// Pixel and occlusion readbacks exercise the render boundary independently of
// the CPU queue. A continuation must retain both earlier pixels and the active
// query; a new logical frame must clear pixels and reset presentation state.
static void testPreviewRenderContinuation(EnginePreviewD3D11& renderer,CDisplayContextD3D11& display) {
    auto white=std::make_shared<AlphaImage>();white->width=white->height=1;white->pixels={255};
    SimpleMesh left;left.texture=white;
    left.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    left.vertices={
        {{-.875f,-.75f,.5f},{0,0},{1,0,0,1}},
        {{-.5f,.75f,.5f},{0,0},{1,0,0,1}},
        {{-.125f,-.75f,.5f},{0,0},{1,0,0,1}}
    };
    left.indices={0,1,2};auto right=left;
    for(auto& vertex:right.vertices) {vertex.position[0]+=1;vertex.color[0]=0;vertex.color[1]=1;}
    auto overlay=left;
    for(auto& vertex:overlay.vertices) {vertex.color[0]=0;vertex.color[2]=1;vertex.color[3]=.5f;}
    auto makeQuery=[] {
        auto query=std::make_shared<WorldQuery>();query->result=std::make_shared<WorldQueryResult>();return query;
    };
    auto waitQuery=[&](const std::shared_ptr<WorldQuery>& query) {
        display.GetContext()->Flush();const auto deadline=GetTickCount64()+5000;
        while(query->result->samples.load()==UINT64_MAX && GetTickCount64()<deadline) {
            renderer.render({});Sleep(1);
        }
        const auto samples=query->result->samples.load();
        require(samples!=UINT64_MAX && samples>0,"Continuation histogram never published visible samples");
        return samples;
    };
    SimpleMesh begin,end;begin.worldQuery=makeQuery();begin.worldQueryBegin=true;end.worldQuery=begin.worldQuery;
    renderer.render({begin,left,right,overlay,end});
    const auto referenceLeft=renderer.readPixel(16,32),referenceRight=renderer.readPixel(48,32);
    nearByte(referenceLeft,0,128);nearByte(referenceLeft,2,128);
    require(referenceRight==0xFF00FF00,
            "Unsplit continuation fixture did not draw both reference triangles");
    const auto unsplitSamples=waitQuery(begin.worldQuery);

    begin.worldQuery=makeQuery();end.worldQuery=begin.worldQuery;
    renderer.render({begin,left},{true,false});
    require(renderer.frameInProgress() && !renderer.worldPresented(),
            "First live part was treated as a completed frame");
    bool blockedRead=false;
    try {renderer.readPixel(16,32);} catch(const std::logic_error&) {blockedRead=true;}
    require(blockedRead,"An unfinished frame was exposed through readback");
    bool blockedCopy=false;
    try {renderer.copyToDisplay();} catch(const std::logic_error&) {blockedCopy=true;}
    require(blockedCopy,"An unfinished frame was copied to the display");
    bool blockedRestart=false;
    try {renderer.render({});} catch(const std::logic_error&) {blockedRestart=true;}
    require(blockedRestart && renderer.frameInProgress(),"A new frame silently replaced an unfinished frame");
    renderer.render({right},{false,false});
    renderer.render({},{false,false});
    require(renderer.frameInProgress(),"Empty continuation prematurely completed the frame");
    renderer.render({overlay,end},{false,true});
    require(!renderer.frameInProgress() && renderer.readPixel(16,32)==referenceLeft &&
            renderer.readPixel(48,32)==referenceRight,"Partitioned frame changed alpha blending or draw order");
    renderer.copyToDisplay();
    require(waitQuery(begin.worldQuery)==unsplitSamples,"Splitting a frame lost or duplicated histogram samples");
    renderer.render({});
    require(renderer.readPixel(16,32)==0xFF000000 && renderer.readPixel(48,32)==0xFF000000,
            "New logical frame retained the previous frame's triangles");

    auto clear=std::make_shared<WorldClear>();clear->targets[0]=0xF3D20000;
    clear->viewport={0,0,64,64};clear->flags=1;clear->color={0,0,1,1};
    auto resolve=std::make_shared<WorldResolve>();resolve->targets=clear->targets;
    resolve->rectangle={0,0,64,64};resolve->destination={0xF3D20001,0,64,64,6};
    SimpleMesh initialize,copy,present;initialize.worldClear=clear;copy.worldResolve=resolve;
    present.worldPresent=std::make_shared<WorldTexture>(resolve->destination);
    renderer.render({initialize},{true,false});renderer.render({copy},{false,false});
    renderer.render({present},{false,false});
    require(renderer.frameInProgress() && !renderer.worldPresented(),
            "A world present command prematurely completed the logical frame");
    renderer.render({},{false,true});
    require(!renderer.frameInProgress() && renderer.worldPresented() && renderer.readPixel(32,32)==0xFFFF0000,
            "Empty continuation reset a completed world presentation");
    renderer.render({});
    require(!renderer.worldPresented() && renderer.readPixel(32,32)==0xFF000000,
            "Default render call did not reset the next logical frame");
    bool blockedContinuation=false;
    try {renderer.render({},{false,true});} catch(const std::logic_error&) {blockedContinuation=true;}
    require(blockedContinuation,"Continuation without an open frame was accepted");
    std::puts("PreviewContinuation: identical blended pixels and histogram samples, split resolve/present, incomplete-copy rejection and new-frame clearing passed.");
}

// Exercise the actual producer/consumer bridge. Queued frames must survive
// guest reuse and texture eviction, and unsupported state must not reuse a
// previous texture. These fixtures are host tests, not captured game frames.
static void testPreviewBridge(Memory& memory, PPCContext& threadContext, EnginePreviewD3D11& renderer) {
    auto* base=memory.base();
    constexpr uint32_t context=0x82A69B00, attributes=context+16896, model=0x01000000;
    constexpr uint32_t descriptor=0x10001, positions=0x11001, colors=0x13001, indices=0x14001, id=0x42;
    require(memory.commit(context,0x4400),"Cannot commit original engine context fixture");
    std::memset(base+context,0,0x4400);
    put32(base,context+8224,model); put32(base,context+16512,descriptor);
    for(unsigned i=0;i<16;++i) {
        putFloat(base,model+16+i*4,i%5==0?1.0f:0.0f);
        putFloat(base,context+17088+i*4,i%5==0?1.0f:0.0f);
    }
    put32(base,context+17160,1280); put32(base,context+17164,720);
    put16(base,attributes+8,id); put32(base,attributes+92,0x01100212);
    put32(base,attributes+108,0xFFFFFFFF); put32(base,attributes+116,0xFFFFFFFF);
    base[attributes+144]=2; base[attributes+145]=1;
    for(unsigned i=0;i<3;++i) put32(base,colors+i*4,0xFFFFFFFF);
    enableEnginePreview();
    auto finish=[] {
        std::vector<SimpleMesh> frame;
        previewEndFrame();
        require(takePreviewFrame(frame),"Completed preview frame was lost");
        std::vector<SimpleMesh> repeated;
        require(!takePreviewFrame(repeated),"Completed frame was delivered twice");
        return frame;
    };
    auto draw=[&] { previewObserveTriangles(base,indices,1); };
    auto count=[&](size_t expected,const char* why) { draw(); require(finish().size()==expected,why); };
    auto upload=[&](uint16_t endpoint) {
        previewPrepareTexture(id); blockFixture(base,0,1,1);
        putLE32(base,colorPixels+16,endpoint); previewObserveImage(base,colorHeader,id,0);
    };
    // The original uploader supplies every authored CImage, in mip order.
    // Distinct colors make replacing authored levels with a filtered base
    // detectable before any GPU upload or later guest-buffer reuse.
    {
        previewPrepareTexture(id);
        const auto resource=colorHeader+256;
        std::memset(base+resource,0,64);put32(base,resource+28,2);put32(base,resource+32,18);
        put32(base,resource+36,3|(3u<<13));put32(base,resource+44,2u<<6);put32(base,resource+48,0x200);
        TextureUpload upload(id,3,0,1);
        const uint16_t endpoints[]{0xF800,0x07E0,0x001F};
        for(unsigned mip=0;mip<3;++mip) {
            blockFixture(base,0,4u>>mip,4u>>mip);
            putLE32(base,colorPixels+16,endpoints[mip]);
            require(upload.record(base,resource,colorHeader,colorPixels+16,mip,0,false) && upload.complete(mip,0),
                    "Original upload slot was not captured and completed");
        }
        previewPublishTexture(id,resource,upload.finish(resource));
        draw();const auto captured=finish();
        require(captured.size()==1 && captured[0].colorTexture,"CPU upload mip fixture was not queued");
        const auto& image=*captured[0].colorTexture;
        std::fprintf(stderr,"CPUUploadMip: captured base=%ux%u lowerLevels=%zu authored=%u\n",
            image.width,image.height,image.mips.size(),unsigned(image.authoredMips));
        require(image.width==4 && image.height==4 && image.authoredMips && image.mips.size()==2 &&
                image.mips[0]==std::vector<uint8_t>({0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255}) &&
                image.mips[1]==std::vector<uint8_t>({0,0,255,255}),
                "CPU image upload discarded authored lower mip levels");
        require(!previewCapturedTexture(id,resource+4),"Upload seed borrowed another resource identity");
        TextureUpload partial(id,3,2,1);
        for(unsigned mip=0;mip<3;++mip) {
            blockFixture(base,0,4u>>mip,4u>>mip);putLE32(base,colorPixels+16,endpoints[mip]);
            if(mip<2)put32(base,colorHeader+40,0x5214);
            require(partial.record(base,resource,colorHeader,colorPixels+16,mip,0,mip<2),"Skipped/resident upload capture failed");
            if(mip==2)require(partial.complete(mip,0),"Resident tail copy witness failed");
        }
        previewPublishTexture(id,resource,partial.finish(resource));
        auto seed=previewCapturedTexture(id,resource);
        require(seed && seed->firstMip==2 && seed->pixels.empty() && seed->mips[0].empty(),"Skipped prefix was rebased or manufactured");
        draw();const auto coarse=finish();renderer.render(coarse);nearByte(renderer.readPixel(32,32),2,255);
        TextureUpload update(id,3,0,1,seed,2);
        for(unsigned mip=0;mip<2;++mip) {
            blockFixture(base,0,4u>>mip,4u>>mip);putLE32(base,colorPixels+16,endpoints[mip]);
            require(update.record(base,resource,colorHeader,colorPixels+16,mip,0,false) && update.complete(mip,0),"Streamed prefix copy failed");
        }
        previewPublishTexture(id,resource,update.finish(resource));
        const auto full=previewCapturedTexture(id,resource);
        require(full && full->firstMip==0 && full->mips[1]==seed->mips[1] && seed->pixels.empty(),"Streaming discarded tail or mutated queued snapshot");
        draw();const auto detailed=finish();renderer.render(detailed);nearByte(renderer.readPixel(32,32),0,255);
        renderer.render(coarse);nearByte(renderer.readPixel(32,32),2,255);
        renderer.render(detailed);nearByte(renderer.readPixel(32,32),0,255);
        std::puts("CPUUploadMip: authored chain, resident-tail preview, incremental prefix and immutable queued generations passed.");
        previewPrepareTexture(id);
    }
    upload(0xF800); draw();
    // Uploaders and temporary engine buffers can change before the window
    // consumes a frame. The bridge must already own every referenced byte.
    putLE32(base,colorPixels+16,0x001F); putFloat(base,positions,200); put16(base,indices,500);
    previewPrepareTexture(id);
    auto red=finish();
    require(red.size()==1 && red[0].opaque && red[0].textureId==id && red[0].colorTexture &&
            red[0].vertices[0].position[0]==-1 && red[0].indices[0]==0 &&
            red[0].colorTexture->pixels==std::vector<uint8_t>({255,0,0,255}),
            "Queued mesh retained guest memory or was destroyed by invalidation");
    renderer.render(red); nearByte(renderer.readPixel(32,32),0,255);
    putFloat(base,positions,-1); put16(base,indices,0);
    count(0,"Invalidated color texture remained available to new draws");
    previewObserveImage(base,colorHeader,id,1);
    count(0,"Non-base mip resurrected invalidated texture");
    previewObserveImage(base,colorHeader,id,0); draw(); auto blue=finish();
    require(blue.size()==1 && blue[0].colorTexture!=red[0].colorTexture,"Rebuilt texture reused old owned pixels");
    renderer.render(blue); nearByte(renderer.readPixel(32,32),2,255);
    renderer.render(red); nearByte(renderer.readPixel(32,32),0,255);

    put32(base,attributes+92,0x01100218); base[attributes+144]=5; base[attributes+145]=6;
    draw(); auto tinted=finish();
    require(tinted.size()==1 && !tinted[0].opaque,"Color material did not select alpha blending");
    auto rejected=[&](auto change,auto restore,const char* why) { change(); count(0,why); restore(); };
    rejected([&]{put32(base,attributes,1);},[&]{put32(base,attributes,0);},"Unsupported shader accepted");
    rejected([&]{base[attributes+145]=1;},[&]{base[attributes+145]=6;},"Unsupported blend accepted");
    rejected([&]{put16(base,attributes+10,2);},[&]{put16(base,attributes+10,0);},"Additional texture accepted");
    rejected([&]{putFloat(base,model+64,1);},[&]{putFloat(base,model+64,0);},"Nonidentity model accepted");
    rejected([&]{put32(base,context+17160,640);},[&]{put32(base,context+17160,1280);},"Partial viewport accepted");
    // Match original 8225D780 selection for both triangle submission paths.
    // A valid inline descriptor must not hide a malformed explicit one or
    // cause a genuine stored-only request to be submitted as immediate data.
    std::memcpy(base+context+12480,base+descriptor,68);
    for(bool decodedPath:{false,true}) {
        auto selectedCount=[&](size_t expected,const char* why) {
            if(decodedPath) previewObserveDecodedTriangles(base,indices,3,3);
            else draw();
            require(finish().size()==expected,why);
        };
        put32(base,context+12564,1);
        selectedCount(1,"Valid explicit CPU descriptor lost priority over stored ID");
        put32(base,context+16512,0);
        selectedCount(0,"Stored-only request fell back to inline CPU data");
        put32(base,context+12564,0);
        selectedCount(1,"Inline CPU fallback with both selectors absent was rejected");
        put32(base,context+16512,descriptor); put32(base,descriptor+4,0);
        selectedCount(0,"Malformed explicit CPU descriptor fell back to inline data");
        put32(base,context+12564,1);
        selectedCount(0,"Malformed explicit CPU descriptor with stored ID was accepted");
        put32(base,descriptor+4,positions); put32(base,context+12564,0);
    }
    rejected([&]{putFloat(base,context+17088,NAN);},[&]{putFloat(base,context+17088,1);},"Nonfinite projection accepted");
    put32(base,descriptor+52,0); count(1,"Verified white uniform fallback was rejected");
    rejected([&]{put32(base,attributes+108,0);},[&]{put32(base,attributes+108,0xFFFFFFFF);},"Missing vertex color ignored nonwhite uniform");
    put32(base,descriptor+52,colors);
    previewObserveTriangles(base,indices,65535); require(finish().empty(),"Batch parent duplicated child geometry");

    // Reuse the same engine ID for another image class and for failed loads.
    previewPrepareTexture(id);
    constexpr uint32_t alphaHeader=0x15001;
    put32(base,alphaHeader+40,0x814); put32(base,alphaHeader+12,8);
    previewObserveImage(base,alphaHeader,id,0); draw(); auto alpha=finish();
    require(alpha.size()==1 && alpha[0].texture && !alpha[0].colorTexture,"Color-to-alpha ID reuse selected stale color");
    upload(0xF800); draw(); auto recolored=finish();
    require(recolored.size()==1 && !recolored[0].texture && recolored[0].colorTexture,"Alpha-to-color ID reuse selected stale alpha");
    previewPrepareTexture(id); putLE32(base,colorPixels,5); previewObserveImage(base,colorHeader,id,0);
    count(0,"Failed replacement retained old texture");

    upload(0xF800);
    for(unsigned i=0;i<513;++i) draw();
    require(finish().size()==512,"Frame mesh limit was not enforced");
    // The live UI path must not inherit the conservative whole-frame cap.
    // Each command has a distinct owned tint, exposing omissions/reordering.
    constexpr unsigned liveGuiDraws=4609;
    withLivePreviewProducer([&] {
        for(unsigned i=0;i<liveGuiDraws;++i) {
            put32(base,colors,0xFF000000u | i);draw();
        }
        previewEndFrame();previewEndFrame();
    },[&](auto&) {
        std::vector<SimpleMesh> commands;
        for(unsigned offset=0;offset<liveGuiDraws;offset+=512) {
            const auto count=(std::min)(512u,liveGuiDraws-offset);
            takeLivePreviewPart(commands,count,offset==0,offset+count==liveGuiDraws);
            for(unsigned i=0;i<count;++i) {
                require(commands[i].colorTexture && commands[i].indices==std::vector<uint16_t>({0,1,2}),
                        "Live GUI queue lost owned texture or indices");
                const auto serial=offset+i;
                require(std::abs(commands[i].vertices[0].color[1]-float((serial>>8)&255)/255)<1e-7f &&
                        std::abs(commands[i].vertices[0].color[2]-float(serial&255)/255)<1e-7f,
                        "Live GUI queue dropped, duplicated or reordered draws");
            }
        }
        takeLivePreviewPart(commands,0,true,true);requireNoPreviewPart(commands);
    });
    put32(base,colors,0xFFFFFFFF);
    std::puts("PreviewStreamingGUI: 4609 ordered draws in 10 bounded parts plus an empty next frame passed.");
    draw(); previewEndFrame(); previewEndFrame();
    std::vector<SimpleMesh> latest;
    require(takePreviewFrame(latest) && latest.empty(),"Empty latest frame replayed an older draw");
    // The live renderer retains ordered frames because a resolve from frame N
    // may be sampled in N+1. Exercise a producer that outruns its consumer.
    setPreviewFrameBackpressure(true);
    draw();previewEndFrame();
    auto producer=std::async(std::launch::async,[]{previewEndFrame();});
    const bool waited=producer.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
    const bool retained=takePreviewFrame(latest) && latest.size()==1;
    const bool delivered=producer.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    // Always release the worker before reporting a failure.
    setPreviewFrameBackpressure(false);producer.get();
    require(waited && retained && delivered,"Live frame delivery discarded an unconsumed frame or stalled its producer");
    require(takePreviewFrame(latest) && latest.empty(),"Live frame order changed");
    setPreviewFrameBackpressure(true);previewEndFrame();
    auto stopping=std::async(std::launch::async,[]{previewEndFrame();});
    setPreviewFrameBackpressure(false);
    require(stopping.wait_for(std::chrono::seconds(2))==std::future_status::ready,"Stopping the consumer stranded its producer");
    stopping.get();takePreviewFrame(latest);
    auto waitingConsumer=std::async(std::launch::async,[]{
        std::vector<SimpleMesh> delivered;return takePreviewFrame(delivered,2000) && delivered.empty();
    });
    const bool consumerWaited=waitingConsumer.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
    previewEndFrame();
    const bool consumerWoke=waitingConsumer.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    require(consumerWaited && consumerWoke && waitingConsumer.get(),"Frame publication did not wake its bounded consumer");
    require(!takePreviewFrame(latest,1),"Bounded consumer replayed an already consumed frame");

    // Exercise recycling while the producer appends the next frame. Empty and
    // differently sized frames must preserve sequence and independent ownership.
    constexpr std::array<unsigned,5> frameSizes{1,17,129,0,3};
    setPreviewFrameBackpressure(true);
    auto cyclingProducer=std::async(std::launch::async,[&] {
        for(unsigned step=0;step<32;++step) {
            for(unsigned i=0;i<frameSizes[step%frameSizes.size()];++i)draw();
            previewEndFrame();
        }
    });
    // Destroy before the future if copying a retained frame throws, so the
    // producer cannot remain blocked while the future's destructor joins it.
    struct BackpressureRelease {
        ~BackpressureRelease() {setPreviewFrameBackpressure(false);}
    } releaseCyclingProducer;
    bool sequence=true;
    std::vector<SimpleMesh> heldFrame;
    for(unsigned step=0;step<32;++step) {
        if(!takePreviewFrame(latest,2000)) {sequence=false;break;}
        sequence &= latest.size()==frameSizes[step%frameSizes.size()];
        if(step==1)heldFrame=latest;
    }
    setPreviewFrameBackpressure(false);cyclingProducer.get();
    require(sequence && heldFrame.size()==17 && heldFrame.front().colorTexture,
            "Recycled command storage lost frame order, empty frames or retained resources");
    const auto retainedTexture=latest.front().colorTexture;
    const auto retainedSize=latest.size();
    require(!takePreviewFrame(latest,1) && latest.size()==retainedSize && latest.front().colorTexture==retainedTexture,
            "Unavailable frame changed the caller's retained commands");
    previewPrepareTexture(id);

    // A frame retains at most four 16 MiB images. Seventeen uploads exceed
    // the separate 256 MiB resident cache while queued references stay valid.
    for(unsigned i=0;i<17;++i) {
        blockFixture(base,0,2048,2048); putLE32(base,colorPixels+16,0xF800);
        previewObserveImage(base,colorHeader,0x500+i,0); put16(base,attributes+8,uint16_t(0x500+i)); draw();
    }
    auto bounded=finish();
    require(bounded.size()==4 && bounded[0].textureId==0x500 && bounded[0].colorTexture->pixels[0]==255,
            "Pending color byte budget or ownership across cache eviction failed");
    put16(base,attributes+8,0x500); count(0,"Oldest color cache entry was not evicted");
    put16(base,attributes+8,0x510); count(1,"Newest color cache entry was evicted");
    for(unsigned i=0;i<17;++i) previewPrepareTexture(0x500+i);
    require(bounded[0].colorTexture->pixels[0]==255,"Cache clear invalidated consumer-owned frame");

    // Run actual AOT list/strip/fan expansion through both the original and
    // wrapped entry. Identical registers, cursor, output count and bytes
    // prove that the observer preserves the original call and output ABI.
    put16(base,attributes+8,id); upload(0xF800);
    put16(base,descriptor,4);
    putFloat(base,positions+36,1); putFloat(base,positions+40,1); putFloat(base,positions+44,0.5f);
    putFloat(base,0x12001+24,0.5f); putFloat(base,0x12001+28,0.5f); put32(base,colors+12,0xFFFFFFFF);
    constexpr uint32_t cursor=0x02000000, countAddress=cursor+32, stream=cursor+64, output=cursor+512;
    auto packet=[&](unsigned type,uint32_t capacity) {
        std::memset(base+cursor,0,1024);
        const unsigned words=type==1?5:6;
        put32(base,cursor,stream); put32(base,cursor+12,stream+words*2); put32(base,countAddress,capacity);
        put16(base,stream,uint16_t((words<<8)|type)); put16(base,stream+2,type==1?1:4);
        for(unsigned i=0;i<(type==1?3u:4u);++i) put16(base,stream+4+i*2,uint16_t(i));
    };
    PPCContext initial{};
    std::memcpy(&initial,&threadContext,sizeof(initial));
    initial.r3.u32=cursor; initial.r4.u32=output; initial.r5.u32=countAddress;
    initial.lr=0x8225E1A0; initial.r19.u64=0x1122334455667788ull; initial.r31.u64=0x8877665544332211ull;
    for(unsigned type:{1u,3u,4u}) {
        packet(type,8192); PPCContext original,wrapped;
        std::memcpy(&original,&initial,sizeof(initial));
        __imp__sub_8225F320(original,base);
        std::array<uint8_t,1024> expected{}; std::memcpy(expected.data(),base+cursor,expected.size());
        packet(type,8192); std::memcpy(&wrapped,&initial,sizeof(initial));
        sub_8225F320(wrapped,base);
        require(!std::memcmp(&original,&wrapped,sizeof(original)) &&
                !std::memcmp(expected.data(),base+cursor,expected.size()),"Decoded-index observer changed original AOT results");
        auto decoded=finish();
        // Original 8225F498 emits the odd strip triangle as next/current/
        // previous: 3,2,1. A cyclic rotation has the same winding but is not
        // the original byte sequence that this observer must preserve.
        const std::vector<uint16_t> expectedIndices=type==1?std::vector<uint16_t>{0,1,2}:
            type==3?std::vector<uint16_t>{0,1,2,3,2,1}:std::vector<uint16_t>{0,1,2,0,2,3};
        if(decoded.size()!=1 || decoded[0].indices!=expectedIndices) {
            std::fprintf(stderr,"Packed type=%u original count=%u native meshes=%zu\n",
                         type,memory.read32(countAddress),decoded.size());
            printPreviewCounters();
        }
        require(decoded.size()==1 && decoded[0].indices==expectedIndices,
                "Original packed list/strip/fan produced missing, duplicated or incorrect native geometry");
    }
    packet(1,8192); PPCContext unrelated=initial; unrelated.lr=0x12345678;
    sub_8225F320(unrelated,base);
    require(finish().empty(),"Decoder observer accepted an unrelated caller");
    packet(3,3); PPCContext shortBuffer=initial; sub_8225F320(shortBuffer,base);
    require(finish().empty(),"Zero-output decoder chunk submitted geometry");
    for(auto limits:{std::array<uint32_t,2>{0,3},{8193,3},{8192,8193},{8192,4}})
        previewObserveDecodedTriangles(base,output,limits[0],limits[1]);
    require(finish().empty(),"Malformed decoded count reached native drawing");
    previewPrepareTexture(id);
    // Multiple material passes can share one large geometry buffer. Retaining
    // it hundreds of times must not exhaust a per-frame memory budget and
    // discard the later original material passes.
    std::array<uint8_t,160> savedAttributes{};std::array<uint8_t,24> savedViewport{};
    std::memcpy(savedAttributes.data(),base+attributes,savedAttributes.size());
    std::memcpy(savedViewport.data(),base+context+17152,savedViewport.size());
    const auto device=memory.allocate(0x4000);require(device!=0,"World queue fixture allocation failed");
    std::memset(base+attributes,0,savedAttributes.size());
    base[attributes+97]=8; // Opaque depth fixture: original alpha comparison ALWAYS.
    putFloat(base,context+17168,0);putFloat(base,context+17172,1);
    EngineVertexBindingSnapshot binding;binding.deviceAddress=device;binding.descriptor.flags=0x01000000;
    binding.descriptor.modes.fill(4);
    const auto encoded=encodeEngineVertexDescriptor(binding.descriptor);
    for(unsigned i=0;i<5;++i)binding.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
        uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
    auto shared=std::make_shared<StoredGeometry>();shared->vertexCount=65535;shared->stride=12;shared->formats[0]=3;
    shared->vertices.resize(size_t(shared->vertexCount)*shared->stride);shared->indices={0,1,2};
    StoredDraw world;world.vertices=shared;world.indices=shared;world.indexCount=3;world.vertexBindings=binding;
    WorldDraw validated;require(snapshotWorldDraw(base,world,validated),"World queue fixture rejected before retention");
    {
        std::array<int32_t,4> rectangle{7,11,39,43};
        put32(base,device+12448,0x1234);
        previewObserveClear(base,device,48,0,.5f,0xAB,rectangle);rectangle[0]=99;
        const auto commands=finish();
        require(commands.size()==1 && commands[0].worldClear && commands[0].worldClear->targets[4]==0x1234 &&
                commands[0].worldClear->rectangle==std::array<int32_t,4>{7,11,39,43} && commands[0].worldClear->depth==.5f &&
                commands[0].worldClear->stencil==0xAB,"Completed clear region or state was not owned by the queued command");
        previewObserveClear(base,device,48,0,.5f,0,rectangle);
        require(finish().empty(),"Inverted clear rectangle reached the native queue");
        const auto oldX=memory.read32(context+17152);put32(base,context+17152,UINT32_MAX);
        previewObserveClear(base,device,48,0,.5f,0,{0,0,1,1});
        require(finish().empty(),"Wrapped clear viewport reached the native renderer");
        put32(base,context+17152,oldX);put32(base,device+12448,0);
    }
    // Original822478C0 copies fragment constants into device+6016 before the
    // draw. The source program block may already have been reused by the GUI.
    const uint32_t program=device+14000,name=device+14128,mutableEnv=device+14400;
    put32(base,program,5);put32(base,program+4,name);put32(base,program+12,mutableEnv);put32(base,program+16,1);
    std::memcpy(base+name,"GUIFadeToWhite",15);put32(base,mutableEnv,0xFFFFFFFF);
    for(unsigned lane=0;lane<4;++lane)putFloat(base,device+6016+lane*4,float(lane+1));
    put32(base,attributes,program);
    require(snapshotWorldDraw(base,world,validated) && validated.fragmentConstants[0]==EngineVector{1,2,3,4},
            "Draw did not retain the fragment constants already uploaded by the original engine");
    put32(base,device+6024,0xFFFFFFFF);
    require(snapshotWorldDraw(base,world,validated) && std::bit_cast<uint32_t>(validated.fragmentConstants[0][2])==0xFFFFFFFF,
            "Original shader-data NaN was changed or caused the whole GUI draw to be dropped");
    put32(base,attributes,0);
    // Complete the original device binding after selecting each fixture
    // resource. Editing a wrapper alone does not rebind an unchanged ID.
    const auto textureFixture=memory.allocate(1024);require(textureFixture!=0,"Texture selection fixture allocation failed");
    const auto savedTable=memory.read32(context+17964);
    const auto resource=textureFixture+64,primary=textureFixture+320,alternate=textureFixture+448;
    put32(base,context+17964,textureFixture);put32(base,textureFixture+8,resource);put16(base,attributes+8,1);
    auto textureHeader=[&](uint32_t object,uint32_t storage) {
        put32(base,object+28,2);put32(base,object+32,storage|6);put32(base,object+36,63|(31<<13));
    };
    for(unsigned s=0;s<16;++s)base[device+11968+s]=15;
    auto bindTexture=[&](unsigned slot,uint32_t object) {
        PPCContext guest;std::memcpy(&guest,&threadContext,sizeof guest);
        guest.r3.u64=device;guest.r4.u64=slot;guest.r5.u64=object;guest.r6.u64=uint64_t(1)<<(31-slot);
        __imp__sub_82864F20(guest,base);
    };
    textureHeader(primary,0x01000000);textureHeader(alternate,0x02000000);
    put32(base,resource+84,primary);put32(base,resource+164,alternate);
    put32(base,resource+172,0x10000000);
    bindTexture(0,primary);
    // Captured Darkness boundary: an unnamed two-stage fixed draw composites
    // the scene and the effect atlas before the full-frame resolve. The old
    // one-stage limit silently omitted this original command.
    {
        std::array<uint8_t,160> savedAttributes{};
        std::array<uint8_t,16> savedConstants{};
        std::memcpy(savedAttributes.data(),base+attributes,savedAttributes.size());
        std::memcpy(savedConstants.data(),base+device+6016,savedConstants.size());
        const auto secondResource=textureFixture+640;
        put32(base,textureFixture+12,secondResource);
        put32(base,secondResource+84,alternate);put32(base,secondResource+172,0x10000000);
        put32(base,attributes,0);put32(base,attributes+92,0x01100210);
        for(unsigned s=0;s<16;++s)put16(base,attributes+8+s*2,0);
        put16(base,attributes+8,1);put16(base,attributes+10,2);
        bindTexture(1,alternate);
        for(unsigned lane=0;lane<4;++lane)putFloat(base,device+6016+lane*4,0);
        WorldDraw composite;
        require(snapshotWorldDraw(base,world,composite) && composite.material==WorldMaterial::fixed &&
                composite.fragmentName=="MRenderXenon_Attrib_TexEnvMode02" && composite.textureMask==3 &&
                composite.textureIds[0]==1 && composite.textureIds[1]==2 &&
                composite.textureObjects[0].object==primary && composite.textureObjects[1].object==alternate &&
                composite.fragmentConstants[0]==EngineVector{},
                "Darkness two-stage composite was dropped or lost a texture/constant");
        put32(base,alternate+32,0x03000006);putFloat(base,device+6016,1);
        require(composite.textureObjects[1].storage==0x02000000 && composite.fragmentConstants[0]==EngineVector{},
                "Darkness composite retained mutable guest texture/constant state");
        textureHeader(alternate,0x02000000);
        put16(base,attributes+8,0);
        require(!snapshotWorldDraw(base,world,validated),"Sparse fixed texture stages were silently renumbered");
        put16(base,attributes+8,1);put16(base,attributes+12,1);
        require(!snapshotWorldDraw(base,world,validated),"An untranslated third fixed stage was accepted");
        std::memcpy(base+attributes,savedAttributes.data(),savedAttributes.size());
        std::memcpy(base+device+6016,savedConstants.data(),savedConstants.size());
        bindTexture(1,0);
        std::puts("DarknessCompositeCapture: two owned texture bindings, uploaded constants and strict stage gates passed.");
    }
    require(worldFragmentTextureMask("XRShader_FP20_NDSP",0)==0x17 &&
            worldFragmentTextureMask("XRShader_FP20_NDS",0)==7 &&
            worldFragmentTextureMask("XRShader_MotionMap",0)==0 &&
            worldFragmentTextureMask("MRenderXenon_Attrib_TexEnvMode00",0)==0 &&
            worldFragmentTextureMask("UnknownNativeFragment",0)==0xFFFF &&
            worldFragmentTextureMask("XRShader_FP20_NDSP",1)==0xFFFF,
            "Original fragment capture masks or conservative unknown fallback changed");
    put16(base,attributes+8+15*2,1);put32(base,device+1152+15*24,2);
    bindTexture(15,primary);
    require(snapshotWorldDraw(base,world,validated) && !validated.textureMask &&
            !validated.textureIds[0] && !validated.textureObjects[0].object && !validated.samplers[15].valid,
            "Depth-only capture retained unused bound textures or sampler state");
    std::strcpy(reinterpret_cast<char*>(base+name),"XRShader_MotionMap");put32(base,attributes,program);
    require(snapshotWorldDraw(base,world,validated) && !validated.textureMask && !validated.textureObjects[0].object,
            "Texture-free motion capture retained stale image bindings");
    std::strcpy(reinterpret_cast<char*>(base+name),"UnknownNativeFragment");
    require(snapshotWorldDraw(base,world,validated) && validated.textureMask==0xFFFF &&
            validated.textureObjects[15].object==primary && validated.samplers[15].valid,
            "Unknown shader capture lost conservative texture/sampler state");
    std::strcpy(reinterpret_cast<char*>(base+name),"GUIFadeToWhite");
    require(snapshotWorldDraw(base,world,validated) && validated.textureMask==1 &&
            !validated.textureIds[15] && !validated.textureObjects[15].object && !validated.samplers[15].valid,
            "Known fragment captured an unused texture slot");
    previewObserveWorld(base,world);
    const auto selective=finish();
    require(selective.size()==1 && selective[0].world && selective[0].world->textureMask==1 &&
            !selective[0].world->textures[15] && !selective[0].world->textureObjects[15].object,
            "Native queue retained an unused fragment texture");
    require(snapshotWorldDraw(base,world,validated) && validated.textureObjects[0].object==primary &&
            validated.textureObjects[0].storage==0x01000000,"Primary completed texture identity was lost");
    put32(base,resource+172,0x12000000);
    bindTexture(0,alternate);
    require(snapshotWorldDraw(base,world,validated) && validated.textureObjects[0].object==alternate &&
            validated.textureObjects[0].storage==0x02000000,"Alternate texture sampled stale primary storage");
    put32(base,resource+164,0);put32(base,resource+88,1);textureHeader(resource+92,0x03000000);
    bindTexture(0,resource+92);
    require(snapshotWorldDraw(base,world,validated) && validated.textureObjects[0].object==resource+92 &&
            validated.textureObjects[0].storage==0x03000000,"Inline alternate texture was not retained");
    put32(base,resource+88,0);
    bindTexture(0,0);
    require(snapshotWorldDraw(base,world,validated) && !validated.textureObjects[0].object,
            "Missing alternate texture fell back to stale primary storage");
    put32(base,resource+172,0x02000000);
    require(snapshotWorldDraw(base,world,validated) && !validated.textureObjects[0].object,
            "Incomplete texture resource was sampled");
    // A missed/evicted upload must recover only genuinely resident mips.
    // A sampler may request a stricter minimum than the original upload skip.
    {
        const auto pixels=memory.allocate(65536);require(pixels!=0,"Resident-cache fixture allocation failed");
        struct ReleasePixels {decltype(memory)& memory;uint32_t address;~ReleasePixels(){memory.release(address);}} releasePixels{memory,pixels};
        std::array<uint8_t,24> oldSampler{};std::memcpy(oldSampler.data(),base+device+1152,oldSampler.size());
        const uint32_t fetch[]{2u|(1u<<22),(pixels+0x8000)|6u,7u|(7u<<13),0xd10u,3u<<6,(pixels+0xa000)|0x200u};
        constexpr std::array<std::array<uint8_t,4>,4> colors{{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}}};
        for(unsigned mip=0;mip<4;++mip) {
            TextureMipLayout layout;require(!getTextureMipLayout(fetch,0,mip,layout),"Resident-cache mip layout failed");
            for(unsigned y=0;y<layout.height;++y)for(unsigned x=0;x<layout.width;++x)
                std::memcpy(base+layout.allocationAddress+layout.surfaceOffsetBytes+y*layout.rowPitchBytes+x*4,colors[mip].data(),4);
        }
        struct Protect {
            uint8_t* address;size_t bytes;DWORD old{};
            Protect(uint8_t* p,size_t n):address(p),bytes(n){require(VirtualProtect(address,bytes,PAGE_NOACCESS,&old)!=0,"Resident-cache guard failed");}
            ~Protect(){DWORD ignored{};VirtualProtect(address,bytes,old,&ignored);}
        };
        for(unsigned variant=0;variant<4;++variant) {
            previewPrepareTexture(1);
            const bool useAlternate=variant&1,inlineHeader=variant&2;
            const uint32_t object=inlineHeader?resource+(useAlternate?92:12):useAlternate?alternate:primary;
            put32(base,resource+(useAlternate?164:84),inlineHeader?0:object);
            put32(base,resource+(useAlternate?88:8),inlineHeader?1:0);
            for(unsigned i=0;i<6;++i)put32(base,object+28+i*4,fetch[i]);
            put32(base,resource+172,(useAlternate?0x12000000:0x10000000)|(inlineHeader?5u<<13:0u));
            putFloat(base,resource+176,2.0f);put32(base,resource+180,2);
            bindTexture(0,object);
            std::strcpy(reinterpret_cast<char*>(base+name),"XRUtil_RenderSurface");
            for(unsigned i=0;i<6;++i)put32(base,device+1152+i*4,i==0?2:i==4?(3u<<2)|(3u<<6):0);
            std::shared_ptr<const ColorImage> tail;
            {
                Protect missingBase(base+pixels+0x8000,4096),missingMip1(base+pixels+0xa000,8192);
                previewObserveWorld(base,world);const auto sparse=finish();
                require(sparse.size()==1 && sparse[0].world && sparse[0].world->textures[0],
                        "Cache recovery read unavailable leading mips instead of the resident tail");
                require(sparse[0].world->textureObjects[0].mipLevels==4 && sparse[0].world->textureObjects[0].firstMip==2,
                        "Captured resource lost declared levels or original pending-prefix state");
                tail=sparse[0].world->textures[0];
                require(tail->valid() && tail->width==8 && tail->height==8 && tail->firstMip==2 &&
                        tail->pixels.empty() && tail->mips[0].empty() && tail->mips[1].size()==16 && tail->mips[2].size()==4,
                        "Cache recovery used sampler restriction as residency or rebased mip indices");
                for(unsigned mip=2;mip<4;++mip)for(size_t offset=0;offset<tail->mips[mip-1].size();offset+=4)
                    require(!std::memcmp(tail->mips[mip-1].data()+offset,colors[mip].data(),4),"Recovered tail pixels changed");
                put32(base,device+1168,(2u<<2)|(3u<<6));
                previewObserveWorld(base,world);const auto reused=finish();
                require(reused.size()==1 && reused[0].world->textures[0]==tail,
                        "Sampler-only LOD change discarded a usable recovered tail");
                put32(base,resource+180,0);put32(base,device+1168,(3u<<2)|(3u<<6));
                previewObserveWorld(base,world);const auto restricted=finish();
                require(restricted.size()==1 && restricted[0].world->textures[0]==tail,
                        "Cache recovery discarded usable tail pixels outside the requested LOD range");
            }
            // Original 82257010 clears +180 after completing the prefix; +176
            // can still carry its fade. Recover newly available levels even if
            // their upload observer was missed, without changing old snapshots.
            put32(base,resource+180,0);put32(base,device+1168,3u<<6);
            previewObserveWorld(base,world);const auto completed=finish();
            require(completed.size()==1 && completed[0].world && completed[0].world->textures[0],
                    "Completed resource could not replace its recovered sparse cache entry");
            const auto full=completed[0].world->textures[0];
            require(full!=tail && full->valid() && full->firstMip==0 && full->pixels.size()==256 &&
                    full->mips[0].size()==64 && full->mips[1]==tail->mips[1] && full->mips[2]==tail->mips[2] &&
                    tail->firstMip==2 && tail->pixels.empty() && tail->mips[0].empty(),
                    "Cache retained a stale sparse generation after prefix completion or mutated its old image");
            for(unsigned mip=0;mip<2;++mip) {
                const auto& level=mip?full->mips[0]:full->pixels;
                for(size_t offset=0;offset<level.size();offset+=4)
                    require(!std::memcmp(level.data()+offset,colors[mip].data(),4),"Recovered prefix pixels changed");
            }
            putFloat(base,resource+176,0);
            for(uint32_t unused:{2u,4u,0xffffffffu}) {
                put32(base,resource+180,unused);
                require(snapshotWorldDraw(base,world,validated) && validated.textureObjects[0].object==object &&
                        validated.textureObjects[0].firstMip==0,"Inactive fade revived or validated an unused pending-prefix count");
            }
            for(unsigned bad=0;bad<4;++bad) {
                putFloat(base,resource+176,bad==0?std::bit_cast<float>(0x7fc00000u):2.0f);
                put32(base,resource+180,bad==1?0xffffffffu:bad==2?4u:2u);
                if(bad==3)put32(base,object+44,1u<<6);
                require(snapshotWorldDraw(base,world,validated) && !validated.textureObjects[0].object,
                        "Invalid upload residency was clamped into a readable texture");
                put32(base,object+44,3u<<6);
            }
        }
        previewPrepareTexture(1);putFloat(base,resource+176,0);put32(base,resource+180,0);
        std::memcpy(base+device+1152,oldSampler.data(),oldSampler.size());
        std::puts("TextureCacheResidency: guarded leading mips, four selected header forms, sampler changes and immutable completed generations passed.");
    }
    // Direct82256008 bypasses initial-upload publication. Updating pixels in
    // the same allocation must evict the old CPU image even if every descriptor
    // field stays identical; dimensions alone cannot detect this generation.
    {
        const auto scratch=memory.allocate(65536),storage=memory.allocate(65536);
        require(scratch && storage,"Original direct-refresh fixture allocation failed");
        struct ReleaseRefresh {decltype(memory)& memory;uint32_t a,b;~ReleaseRefresh(){memory.release(a);memory.release(b);}} releaseRefresh{memory,scratch,storage};
        const uint32_t source=scratch,vtable=scratch+256,metadata=scratch+512,view=scratch+1024,output=scratch+2048;
        const uint32_t tls=memory.read32(threadContext.r13.u32),callback=uint32_t(PPC_CODE_BASE);
        const auto oldTlsView=memory.read32(tls+8),oldTlsSize=memory.read32(tls+16);
        auto* oldFunction=PPC_LOOKUP_FUNC(base,callback);
        std::array<uint8_t,1280> oldStack{};std::memcpy(oldStack.data(),base+threadContext.r1.u32-1024,oldStack.size());
        std::array<uint8_t,24> oldSampler{};std::memcpy(oldSampler.data(),base+device+1152,oldSampler.size());
        struct RestoreRefresh {
            uint8_t* base;uint32_t tls,oldView,oldSize,callback,stack;PPCFunc* oldFunction;const std::array<uint8_t,1280>& bytes;
            ~RestoreRefresh(){put32(base,tls+8,oldView);put32(base,tls+16,oldSize);PPC_LOOKUP_FUNC(base,callback)=oldFunction;
                std::memcpy(base+stack,bytes.data(),bytes.size());}
        } restoreRefresh{base,tls,oldTlsView,oldTlsSize,callback,threadContext.r1.u32-1024,oldFunction,oldStack};
        PPC_LOOKUP_FUNC(base,callback)=refreshSource;
        std::strcpy(reinterpret_cast<char*>(base+name),"XRUtil_RenderSurface");
        for(unsigned i=0;i<6;++i)put32(base,device+1152+i*4,i==0?2:0);
        putFloat(base,resource+176,0);put32(base,resource+180,0);put16(base,resource+168,1);base[resource+170]=0;
        for(unsigned variant=0;variant<4;++variant) {
            previewPrepareTexture(1);
            const bool useAlternate=variant&1,inlineHeader=variant&2;
            const uint32_t object=inlineHeader?resource+(useAlternate?92:12):useAlternate?alternate:primary;
            put32(base,resource+(useAlternate?164:84),inlineHeader?0:object);
            put32(base,resource+(useAlternate?88:8),inlineHeader?1:0);
            put32(base,resource+172,useAlternate?0x1a000000:0x18000000);
            std::memset(base+object,0,64);put32(base,object+4,1);
            const uint32_t fetch[]{2u|(2u<<22),storage|0x86u,63u|(31u<<13),0x1414u,0u,0x200u};
            for(unsigned i=0;i<6;++i)put32(base,object+28+i*4,fetch[i]);
            std::memset(base+storage,31,65536);
            bindTexture(0,object);
            previewObserveWorld(base,world);const auto before=finish();
            require(before.size()==1 && before[0].world && before[0].world->textures[0],"Direct-refresh initial recovery failed");
            const auto old=before[0].world->textures[0];const auto oldPixels=old->pixels;
            require(old->width==64 && old->height==32 && oldPixels[0]==31 && oldPixels[3]==255,
                    "Direct-refresh initial decoder pixels differ");
            put32(base,source,vtable);put32(base,vtable+124,callback);put32(base,source+4,object);
            put32(base,source+8,storage);put32(base,source+12,view);put32(base,source+16,0);
            put32(base,metadata+32,64);std::memset(base+view,0,48);
            put32(base,tls+8,view);put32(base,tls+16,48);
            PPCContext refresh;std::memcpy(&refresh,&threadContext,sizeof(refresh));
            refresh.r3.u64=resource;refresh.r4.u64=source;refresh.r5.u64=7;refresh.r6.u64=metadata;
            refresh.r9.u64=64;refresh.r10.u64=32;
            put32(base,refresh.r1.u32+84,1);put32(base,refresh.r1.u32+92,0);put32(base,refresh.r1.u32+100,0);
            put32(base,refresh.r1.u32+116,output);
            sub_82256008(refresh,base);
            require(refresh.r3.u32==1 && refresh.r1.u32==threadContext.r1.u32 && memory.read32(source+16)==1 &&
                    memory.read32(output)==0x13579bdf,"Original direct refresh did not complete its source callback");
            for(unsigned i=0;i<6;++i)require(memory.read32(object+28+i*4)==fetch[i],"Direct refresh unexpectedly recreated the descriptor");
            previewObserveWorld(base,world);const auto after=finish();
            require(after.size()==1 && after[0].world && after[0].world->textures[0],"Direct-refresh replacement recovery failed");
            const auto fresh=after[0].world->textures[0];
            require(fresh!=old && fresh->width==64 && fresh->height==32 && fresh->pixels.size()==8192,
                    "Original direct refresh reused the cached pre-update image");
            for(size_t i=0;i<fresh->pixels.size();i+=4)
                require(fresh->pixels[i]==177 && fresh->pixels[i+1]==177 && fresh->pixels[i+2]==177 && fresh->pixels[i+3]==255,
                        "Direct refresh did not recover the changed allocation pixels");
            require(old->pixels==oldPixels,"Direct refresh mutated an already queued image");
            put32(base,object+4,1); // Release fixture-retained view ownership without the original heap.
        }
        previewPrepareTexture(1);std::memcpy(base+device+1152,oldSampler.data(),oldSampler.size());
        std::puts("OriginalTextureRefresh: four selected header forms replace same-allocation pixels and preserve queued generations.");
    }
    // Descriptor reuse must not make a cached image's dimensions override the
    // selected resource. Model the completed state after a missed direct refresh;
    // old queued draws still own their prior image generation.
    {
        const auto storage=memory.allocate(131072);require(storage!=0,"Descriptor refresh fixture allocation failed");
        struct ReleaseStorage {decltype(memory)& memory;uint32_t address;~ReleaseStorage(){memory.release(address);}} releaseStorage{memory,storage};
        std::array<uint8_t,24> oldSampler{};std::memcpy(oldSampler.data(),base+device+1152,oldSampler.size());
        std::strcpy(reinterpret_cast<char*>(base+name),"XRUtil_RenderSurface");
        for(unsigned i=0;i<6;++i)put32(base,device+1152+i*4,i==0?2:0);
        putFloat(base,resource+176,0);put32(base,resource+180,0);
        auto writeResource=[&](uint32_t object,unsigned width,unsigned height,uint32_t address,std::array<uint8_t,4> color) {
            const uint32_t fetch[]{2u|(((width+31)/32)<<22),address|6u,(width-1)|((height-1)<<13),0xd10u,0u,0x200u};
            for(unsigned i=0;i<6;++i)put32(base,object+28+i*4,fetch[i]);
            TextureMipLayout layout;require(!getTextureMipLayout(fetch,0,0,layout),"Descriptor refresh layout failed");
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
                std::memcpy(base+layout.allocationAddress+layout.surfaceOffsetBytes+y*layout.rowPitchBytes+x*4,color.data(),4);
        };
        for(unsigned variant=0;variant<4;++variant) {
            previewPrepareTexture(1);
            const bool useAlternate=variant&1,inlineHeader=variant&2;
            const uint32_t object=inlineHeader?resource+(useAlternate?92:12):useAlternate?alternate:primary;
            put32(base,resource+(useAlternate?164:84),inlineHeader?0:object);
            put32(base,resource+(useAlternate?88:8),inlineHeader?1:0);
            put32(base,resource+172,useAlternate?0x12000000:0x10000000);
            writeResource(object,64,32,storage,{23,47,71,255});
            bindTexture(0,object);
            previewObserveWorld(base,world);const auto before=finish();
            require(before.size()==1 && before[0].world && before[0].world->textures[0],"Initial descriptor refresh recovery failed");
            const auto old=before[0].world->textures[0];
            require(old->width==64 && old->height==32 && old->pixels.size()==8192,"Initial descriptor refresh image dimensions differ");
            const auto oldPixels=old->pixels;
            // A per-binding exponent change does not replace source pixels.
            put32(base,object+40,0xd10u|(4u<<13));
            bindTexture(0,object);
            previewObserveWorld(base,world);const auto rebound=finish();
            require(rebound.size()==1 && rebound[0].world->textures[0]==old,"Binding exponent needlessly replaced cached source pixels");
            writeResource(object,128,64,storage+65536,{89,113,137,255});
            bindTexture(0,object);
            previewObserveWorld(base,world);const auto after=finish();
            require(after.size()==1 && after[0].world && after[0].world->textures[0],"Changed descriptor recovery failed");
            const auto fresh=after[0].world->textures[0];
            require(fresh!=old && fresh->width==128 && fresh->height==64 && fresh->pixels.size()==32768,
                    "Same-object descriptor resize reused stale cached dimensions");
            for(size_t i=0;i<fresh->pixels.size();i+=4)
                require(fresh->pixels[i]==89 && fresh->pixels[i+1]==113 && fresh->pixels[i+2]==137 && fresh->pixels[i+3]==255,
                        "Descriptor resize retained pixels from the previous allocation");
            require(old->width==64 && old->height==32 && old->pixels==oldPixels,
                    "Descriptor resize mutated an already queued image generation");
        }
        previewPrepareTexture(1);std::memcpy(base+device+1152,oldSampler.data(),oldSampler.size());
        std::puts("TextureDescriptorRefresh: four selected header forms recover resized allocations and retain immutable queued images.");
    }
    // Replacement must release the prior entry's budget before considering
    // eviction. Keep an unrelated oldest entry at a nearly full cache.
    {
        constexpr uint32_t sentinelId=0x777, sentinelResource=0x777777;
        auto sentinel=std::make_shared<ColorImage>();sentinel->width=sentinel->height=1;
        sentinel->pixels={11,22,33,255};
        previewPublishTexture(sentinelId,sentinelResource,sentinel);
        auto large=std::make_shared<ColorImage>();large->width=large->height=2048;
        large->pixels.resize(2048*2048*4,0);
        for(unsigned i=0;i<14;++i)previewPublishTexture(0x780+i,0x780000+i,large);
        previewPublishTexture(1,primary,large);
        const auto storage=memory.allocate(2048*2048*4);
        require(storage!=0 && !(storage&4095),"Cache replacement pixel allocation failed");
        std::memset(base+storage,0,2048*2048*4);
        const uint32_t fetch[]{2u|(64u<<22),storage|6u,2047u|(2047u<<13),0xd10u,0u,0x200u};
        for(unsigned i=0;i<6;++i)put32(base,alternate+28+i*4,fetch[i]);
        put32(base,resource+164,alternate);put32(base,resource+172,0x12000000);
        bindTexture(0,alternate);
        std::strcpy(reinterpret_cast<char*>(base+name),"XRUtil_RenderSurface");
        previewObserveWorld(base,world);const auto recovered=finish();
        require(recovered.size()==1 && recovered[0].world && recovered[0].world->textures[0] &&
                previewCapturedTexture(1,alternate)==recovered[0].world->textures[0],
                "Resource replacement did not exercise native cache recovery");
        require(previewCapturedTexture(sentinelId,sentinelResource)==sentinel,
                "Resource replacement leaked cache bytes and evicted an unrelated texture");
        for(unsigned i=0;i<14;++i)previewPrepareTexture(0x780+i);
        previewPrepareTexture(1);previewPrepareTexture(sentinelId);
        require(memory.release(storage),"Cache replacement pixel cleanup failed");
        std::puts("TextureCacheReplacement: recovered resource retains unrelated cache entry at capacity.");
    }
    // Replacing a texture in the upload cache must retain every generation
    // queued for this frame. Two slots and repeated passes share each image;
    // only the seventeenth unique 16 MiB image crosses the 256 MiB part bound.
    {
        previewPrepareTexture(1);
        put32(base,resource+84,primary);put32(base,resource+172,0x10000000);
        putFloat(base,resource+176,0);put32(base,resource+180,0);
        const uint32_t fetch[]{2u|(64u<<22),0x01000000u|6u,2047u|(2047u<<13),0xd10u,0u,0x200u};
        for(unsigned i=0;i<6;++i)put32(base,primary+28+i*4,fetch[i]);
        std::strcpy(reinterpret_cast<char*>(base+name),"XRShader_FP20_NDSP");
        // This shader requires four uploaded vectors; the preceding GUI
        // fixture used only one. Initialize the complete material contract.
        put32(base,attributes,program);put32(base,program+16,4);
        std::memset(base+device+6016,0,4*16);
        put16(base,attributes+8,1);put16(base,attributes+10,1);
        bindTexture(0,primary);bindTexture(1,primary);
        require(snapshotWorldDraw(base,world,validated) && validated.material==WorldMaterial::ndsp &&
                validated.textureObjects[0].object==primary && validated.textureObjects[1].object==primary,
                "Image-budget fixture did not capture its shader and shared texture bindings");
        std::weak_ptr<const ColorImage> retiredImage;
        withLivePreviewProducer([&] {
            for(unsigned i=0;i<17;++i) {
                auto image=std::make_shared<ColorImage>();image->width=image->height=2048;
                image->pixels.resize(2048*2048*4,uint8_t(i+1));
                if(!i)retiredImage=image;
                previewPublishTexture(1,primary,image);
                previewObserveWorld(base,world);previewObserveWorld(base,world);
            }
            previewEndFrame();
        },[&](auto&) {
            std::vector<SimpleMesh> commands;
            takeLivePreviewPart(commands,32,true,false);
            for(unsigned i=0;i<32;++i) {
                const auto& draw=commands[i].world;
                require(draw && draw->textures[0] && draw->textures[0]==draw->textures[1] &&
                        draw->textures[0]->bytes()==16*1024*1024 &&
                        draw->textures[0]->pixels.front()==i/2+1 &&
                        draw->textures[0]==commands[i^1].world->textures[0],
                        "Image-budget split double-charged shared textures or changed a queued generation");
            }
            require(!retiredImage.expired(),"Upload replacement invalidated a consumed image generation");
            takeLivePreviewPart(commands,2,false,true);
            require(retiredImage.expired() && commands[0].world && commands[1].world &&
                    commands[0].world->textures[0]->pixels.front()==17 &&
                    commands[0].world->textures[0]==commands[1].world->textures[0],
                    "Image-budget split lost the next draw or retained a retired generation");
            requireNoPreviewPart(commands);
        });
        previewPrepareTexture(1);put16(base,attributes+10,0);
        std::puts("PreviewStreamingImages: 17 unique generations, shared slots/passes, 256 MiB part bound and owned retirement passed.");
    }
    put32(base,context+17964,savedTable);put16(base,attributes+8,0);put16(base,attributes+8+15*2,0);put32(base,attributes,0);
    constexpr unsigned passes=640;
    for(unsigned i=0;i<passes;++i)previewObserveWorld(base,world);
    auto complete=finish();
    require(complete.size()==passes,"Repeated shared world geometry dropped material passes");
    for(unsigned i=0;i<passes;++i)require(complete[i].world && complete[i].world->geometry.vertices==shared,
        "World queue copied or replaced shared geometry");
    // Distinct buffers still consume the memory budget. A frame fits 64 of
    // these resources; the rejected 65th must be accepted in the next frame.
    for(unsigned i=0;i<65;++i) {
        auto unique=std::make_shared<StoredGeometry>(*shared);
        world.vertices=unique;world.indices=unique;previewObserveWorld(base,world);
    }
    auto boundedWorld=finish();
    require(boundedWorld.size()==64,"Unique world geometry did not enforce its per-frame budget");
    previewObserveWorld(base,world);auto next=finish();
    require(next.size()==1 && next[0].world->geometry.vertices==world.vertices,
        "Frame boundary did not release the world geometry budget");
    uint64_t measured=99;require(!previewReadHistogram(0,measured) && measured==99,"Unavailable histogram fabricated a count");
    previewBeginHistogram(0);previewObserveWorld(base,world);previewEndHistogram();auto queried=finish();
    require(queried.size()==3 && queried[0].worldQueryBegin && queried[0].worldQuery && queried[1].world &&
        queried[2].worldQuery==queried[0].worldQuery && !queried[2].worldQueryBegin,"Histogram boundaries lost draw ordering/ownership");
    queried[0].worldQuery->result->samples.store(1152);
    require(previewReadHistogram(0,measured) && measured==1152,"GPU histogram publication did not reach the producer");
    previewBeginHistogram(0);previewEndHistogram();auto pendingQuery=finish();
    require(previewReadHistogram(0,measured) && measured==1152,"New pending query erased the prior completed histogram");
    pendingQuery[0].worldQuery->result->samples.store(0);
    require(previewReadHistogram(0,measured) && measured==0,"Measured zero was mistaken for an unavailable histogram");
    // A full logical frame exceeds the former 4096-command cap. Cross part
    // boundaries with depth/fixed draws, clears, resolves and a live query.
    const auto destination=device+15000;
    put32(base,destination+32,0x03000000u|6);put32(base,destination+36,63|(63<<13));
    world.vertices=shared;world.indices=shared;
    constexpr unsigned liveGroups=1800,liveCommands=2+liveGroups*3+1;
    withLivePreviewProducer([&] {
        previewBeginHistogram(2);
        for(unsigned i=0;i<liveGroups;++i) {
            put32(base,attributes+92,i%2?0x01100212:0);
            previewObserveWorld(base,world);
            previewObserveClear(base,device,48,0,.5f,i,{0,0,64,64});
            previewObserveResolve(base,device,0,0,destination,0,0,.5f,i,0,0);
        }
        previewEndHistogram();previewObservePresent(base,destination);previewEndFrame();
        previewEndFrame();previewObserveWorld(base,world);previewEndFrame();
    },[&](auto&) {
        std::vector<SimpleMesh> commands;std::shared_ptr<WorldQuery> query;
        for(unsigned offset=0;offset<liveCommands;offset+=512) {
            const auto count=(std::min)(512u,liveCommands-offset);
            takeLivePreviewPart(commands,count,offset==0,offset+count==liveCommands);
            for(unsigned i=0;i<count;++i) {
                const auto ordinal=offset+i;const auto& command=commands[i];
                if(ordinal==0) {
                    require(command.worldQuery && command.worldQueryBegin,"Live frame lost histogram begin");
                    query=command.worldQuery;
                } else if(ordinal==liveCommands-2) {
                    require(command.worldQuery==query && !command.worldQueryBegin,"Live frame lost the matching histogram end");
                } else if(ordinal==liveCommands-1) {
                    require(command.worldPresent && command.worldPresent->object==destination,"Live frame lost its final frontbuffer");
                } else {
                    const unsigned group=(ordinal-1)/3,kind=(ordinal-1)%3;
                    if(kind==0)require(command.world && command.world->geometry.vertices==shared &&
                        command.world->geometry.indices==shared && command.world->geometry.indexCount==3 &&
                        command.world->material==(group%2?WorldMaterial::fixed:WorldMaterial::depth),
                        "Live frame changed world geometry or material pass order");
                    if(kind==1)require(command.worldClear && command.worldClear->stencil==group,
                        "Live frame lost or reordered a clear");
                    if(kind==2)require(command.worldResolve && command.worldResolve->stencil==group &&
                        command.worldResolve->destination.object==destination,"Live frame lost or reordered a resolve");
                }
            }
        }
        takeLivePreviewPart(commands,0,true,true);takeLivePreviewPart(commands,1,true,true);
        require(commands[0].world && commands[0].world->geometry.vertices==shared,"Next live frame lost its geometry");
        requireNoPreviewPart(commands);
    });
    put32(base,attributes+92,0);
    // Split on retained unique vertices even when the command limit is far
    // away. Each producer-owned resource can disappear after publication.
    std::weak_ptr<const StoredGeometry> retired;
    withLivePreviewProducer([&] {
        for(unsigned i=0;i<65;++i) {
            auto unique=std::make_shared<StoredGeometry>(*shared);unique->id=i+1;
            if(!i)retired=unique;
            auto request=world;request.vertices=unique;request.indices=unique;
            previewObserveWorld(base,request);
        }
        previewEndFrame();
    },[&](auto&) {
        std::vector<SimpleMesh> commands;
        takeLivePreviewPart(commands,64,true,false);
        for(unsigned i=0;i<64;++i)require(commands[i].world && commands[i].world->geometry.vertices->id==i+1,
            "Unique-vertex split lost its owned resource or order");
        require(!retired.expired(),"Consumed part did not retain its owned geometry");
        takeLivePreviewPart(commands,1,false,true);
        require(retired.expired() && commands[0].world->geometry.vertices->id==65,
            "Retired part leaked ownership or rejected the resource following the vertex bound");
        requireNoPreviewPart(commands);
    });
    // Shared VB and separate index-only resources are charged independently.
    // Four valid 16 MiB index buffers exceed 64 MiB once the VB is included.
    auto indexOnly=std::make_shared<StoredGeometry>();indexOnly->indices.resize(8*1024*1024,0);
    withLivePreviewProducer([&] {
        for(unsigned i=0;i<4;++i) {
            auto ownedIndices=std::make_shared<StoredGeometry>(*indexOnly);ownedIndices->id=100+i;
            auto request=world;request.indices=ownedIndices;previewObserveWorld(base,request);
        }
        previewEndFrame();
    },[&](auto&) {
        std::vector<SimpleMesh> commands;
        takeLivePreviewPart(commands,3,true,false);
        for(unsigned i=0;i<3;++i)require(commands[i].world->geometry.vertices==shared &&
            commands[i].world->geometry.indices->id==100+i,"Raw geometry-byte split changed separate VB/IB ownership");
        takeLivePreviewPart(commands,1,false,true);
        require(commands[0].world->geometry.indices->id==103,"Raw geometry-byte split discarded the next draw");
    });
    // Stop a producer that has filled the ready slot and is waiting to hand
    // over a second part. Disabling backpressure must unblock clean teardown.
    withLivePreviewProducer([&] {
        for(unsigned i=0;i<1537;++i)previewObserveWorld(base,world);
        previewEndFrame();
    },[&](auto& producer) {
        require(producer.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,
                "Live producer overwrote a part without backpressure");
        setPreviewFrameBackpressure(false);
    });
    std::vector<SimpleMesh> stopped;
    require(takePreviewFrame(stopped),"Shutdown failed to release the final queued part");
    requireNoPreviewPart(stopped);
    std::puts("PreviewStreamingWorld: 5403 ordered commands, 11 parts, histogram/clear/resolve, vertex/byte bounds, resource retirement and blocked-producer shutdown passed.");
    std::memcpy(base+attributes,savedAttributes.data(),savedAttributes.size());
    std::memcpy(base+context+17152,savedViewport.data(),savedViewport.size());
    require(memory.release(device),"World queue fixture cleanup failed");
    std::puts("Original packed geometry and shared world queue retention passed.");
    std::puts("Preview bridge passed: owned snapshots, ID reuse, base mip, material rejection, queue limits and color LRU eviction.");
}

int main(int argc, char** argv) {
    HWND window=nullptr;
    try {
        testDecodedDrawRequest();
        require(argc==2,"Game directory required for original AOT decoder comparison");
        // Match the executable's initialization, including linking the
        // strong engine observers out of DarkRuntime's static archive.
        configureRenderTrace({});
        Memory owner;
        memory=&owner;
        // The original packed decoder calls memcpy, whose tail loop reads
        // executable data. Use the hash-verified image and normal stack/TLS
        // setup instead of invoking AOT against a zero-filled address space.
        owner.load(argv[1]);
        PPCContext ctx{};
        owner.initThread(ctx);
        auto* base=owner.base();
        std::array<uint8_t,32> memoryProbe{};
        require(!copyRenderMemory(base,0xFFFFFFFFull,memoryProbe.data(),2) &&
                !copyRenderMemory(base,0x100000000ull,memoryProbe.data(),1) &&
                !copyRenderMemory(base,UINT64_MAX,memoryProbe.data(),2) &&
                !copyRenderMemory(base,1,memoryProbe.data(),SIZE_MAX),"Wrapped render memory range was accepted");
        DWORD probeProtection=0,probeIgnored=0;
        std::memset(base+0x11FF0,0,memoryProbe.size());
        require(equalRenderMemory(base,0x11FF0,memoryProbe.data(),memoryProbe.size()),"Equal guarded bytes did not compare equal");
        base[0x11FF0]=1;
        require(!equalRenderMemory(base,0x11FF0,memoryProbe.data(),memoryProbe.size()),"Changed render memory compared equal");
        base[0x11FF0]=0;
        require(!equalRenderMemory(base,UINT64_MAX,memoryProbe.data(),2) &&
                !equalRenderMemory(base,1,memoryProbe.data(),SIZE_MAX),"Wrapped render comparison was accepted");
        require(VirtualProtect(base+0x12000,4096,PAGE_NOACCESS,&probeProtection),"Cannot protect render-copy test page");
        require(!equalRenderMemory(base,0x11FF0,memoryProbe.data(),memoryProbe.size()),"Cross-page comparison accepted inaccessible data");
        require(!copyRenderMemory(base,0x11FF0,memoryProbe.data(),memoryProbe.size()),"Cross-page render copy accepted inaccessible data");
        VirtualProtect(base+0x12000,4096,probeProtection,&probeIgnored);
        const uint32_t descriptor=0x10001, positions=0x11001, uv=0x12001, colors=0x13001, indices=0x14001, image=0x15001, pixels=0x16001;
        std::memset(base+descriptor,0,68);
        put16(base,descriptor,3); put32(base,descriptor+4,positions); put32(base,descriptor+8,uv);
        put32(base,descriptor+52,colors); base[descriptor+40]=2;
        const float vertices[3][3]={{-1,-1,0.5f},{0,1,0.5f},{1,-1,0.5f}};
        for(unsigned i=0;i<3;++i) {
            for(unsigned j=0;j<3;++j) putFloat(base,positions+i*12+j*4,vertices[i][j]);
            putFloat(base,uv+i*8,0.5f); putFloat(base,uv+i*8+4,0.5f);
            put32(base,colors+i*4,0xFFFF8040);
            put16(base,indices+i*2,uint16_t(i));
        }
        SimpleMesh mesh;
        require(!decodeSimpleMesh(base,descriptor,indices,1,mesh), "Cannot decode valid unaligned BE mesh");
        require(mesh.vertices.size()==3 && mesh.indices==std::vector<uint16_t>({0,1,2}), "Incorrect topology decode");
        require(mesh.vertices[1].position[1]==1 && mesh.vertices[0].uv[0]==0.5f, "Incorrect float endian conversion");
        require(mesh.vertices[0].color[0]==1 && std::abs(mesh.vertices[0].color[1]-128.0f/255)<1e-7f &&
                std::abs(mesh.vertices[0].color[2]-64.0f/255)<1e-7f && mesh.vertices[0].color[3]==1, "Wrong D3DCOLOR channels");
        ctx.r3.u32=descriptor;
        sub_82253D10(ctx,base);
        require(ctx.r3.u32==24, "Original engine stream-size helper disagrees with decoded layout");
        put16(base,indices+4,3);
        require(decodeSimpleMesh(base,descriptor,indices,1,mesh)!=nullptr && mesh.indices[2]==2, "Invalid index changed previous mesh");
        put16(base,indices+4,2);
        require(decodeSimpleMesh(base,descriptor,indices,65535,mesh)!=nullptr, "Batch sentinel treated as triangle count");
        putFloat(base,positions,NAN);
        require(decodeSimpleMesh(base,descriptor,indices,1,mesh)!=nullptr, "Accepted nonfinite position");
        putFloat(base,positions,-1);
        base[descriptor+40]=3;
        require(decodeSimpleMesh(base,descriptor,indices,1,mesh)!=nullptr, "Accepted unsupported UV layout");
        base[descriptor+40]=2;
        DWORD oldProtection=0;
        require(VirtualProtect(base+0x12000,4096,PAGE_NOACCESS,&oldProtection), "Cannot prepare unreadable UV page");
        require(decodeSimpleMesh(base,descriptor,indices,1,mesh)!=nullptr, "Accepted inaccessible UV page");
        DWORD ignored=0; VirtualProtect(base+0x12000,4096,oldProtection,&ignored);
        put32(base,descriptor+4,0xFFFFFFF8);
        require(decodeSimpleMesh(base,descriptor,indices,1,mesh)!=nullptr, "Accepted wrapped position span");
        put32(base,descriptor+4,positions);
        put32(base,descriptor+52,0);
        require(decodeSimpleMesh(base,descriptor,indices,1,mesh)!=nullptr,"Text path accepted missing color stream");
        SimpleMesh videoMesh;
        require(!decodeSimpleMesh(base,descriptor,indices,1,videoMesh,true) && videoMesh.vertices[0].color[0]==1 &&
                videoMesh.vertices[0].color[3]==1,"Explicit white video vertex color not decoded");
        ctx.r3.u32=descriptor; sub_82253D10(ctx,base);
        require(ctx.r3.u32==20,"Original engine disagrees with video position/UV-only layout");
        put32(base,descriptor+52,colors);

        std::memset(base+image,0,48);
        put32(base,image,0x82097610); put32(base,image+8,pixels); put32(base,image+12,8);
        put32(base,image+16,2); put32(base,image+20,2); put32(base,image+24,4);
        put32(base,image+28,1); put32(base,image+32,0x40000); put32(base,image+40,0x814);
        const uint8_t rows[8]={0,255,99,99,64,128,88,88}; std::memcpy(base+pixels,rows,8);
        AlphaImage alpha;
        require(!decodeAlphaImage(base,image,alpha) && alpha.pixels==std::vector<uint8_t>({0,255,64,128}), "Alpha row-pitch conversion failed");
        put32(base,image+40,0x1814);
        require(decodeAlphaImage(base,image,alpha)!=nullptr && alpha.pixels[1]==255, "Accepted compressed image or changed previous image");
        put32(base,image+40,0x814); put32(base,image+12,7);
        require(decodeAlphaImage(base,image,alpha)!=nullptr, "Accepted undersized pixel storage");
        put32(base,image+12,8); put32(base,image+8,0xFFFFFFFE);
        require(decodeAlphaImage(base,image,alpha)!=nullptr, "Accepted wrapped image span");
        put32(base,image+8,pixels); put32(base,image+40,0x200814);
        require(decodeAlphaImage(base,image,alpha)!=nullptr,"Alignment padding escaped image allocation size");
        put32(base,image+12,39);
        std::memcpy(base+0x16020,rows,8);
        require(!decodeAlphaImage(base,image,alpha) && alpha.pixels==std::vector<uint8_t>({0,255,64,128}),
                "Original 32-byte image-lock alignment not honored");

        const uint32_t videoFrame=0x17001;
        std::memset(base+videoFrame,0,12);
        auto plane=[&](uint32_t at,uint32_t width,uint32_t height,uint32_t pitch,uint32_t bpp,uint32_t format,uint32_t data,uint32_t size) {
            std::memset(base+at,0,48); put32(base,at,0x82097610); put32(base,at+8,data); put32(base,at+12,size);
            put32(base,at+16,width); put32(base,at+20,height); put32(base,at+24,pitch); put32(base,at+28,bpp);
            put32(base,at+32,format); put32(base,at+40,0x810);
        };
        plane(videoFrame+12,2,2,4,1,0x2000,0x18001,8);
        plane(videoFrame+60,1,1,4,2,0x20000,0x19001,4);
        const uint8_t lumaRows[]={16,235,77,77,81,145,66,66}; std::memcpy(base+0x18001,lumaRows,8);
        base[0x19001]=90; base[0x19002]=240;
        VideoFrame decodedVideo;
        require(!decodeVideoFrame(base,videoFrame,decodedVideo) && decodedVideo.luma==std::vector<uint8_t>({16,235,81,145}) &&
                decodedVideo.chroma==std::vector<uint8_t>({90,240}),"Original Y/UV plane pitch or ordering changed");
        put32(base,videoFrame+60+20,2); put32(base,videoFrame+60+12,8);
        require(decodeVideoFrame(base,videoFrame,decodedVideo)!=nullptr && decodedVideo.chroma[1]==240,"Chroma size mismatch accepted");
        put32(base,videoFrame+60+20,1); put32(base,videoFrame+60+12,4);
        put32(base,videoFrame+12+8,0xFFFFFFFE);
        require(decodeVideoFrame(base,videoFrame,decodedVideo)!=nullptr,"Wrapped video pixel span accepted");
        require(decodeVideoFrame(base,0xFFFFFFD0,decodedVideo)!=nullptr,"Wrapped video image header accepted");
        testColorDecode(base);
        testPromptIcons();
        testPromptOriginFixture(owner);

        // Actual D3D11 draw: half-alpha atlas, endian-converted vertex tint,
        // index buffer, matrix constant and blending all affect this readback.
        window=CreateWindowExW(0,L"STATIC",L"Engine mesh contract",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window!=nullptr,"Cannot create native test window");
        {
            CDisplayContextD3D11 display; display.Init(window,64,64);
            EnginePreviewD3D11 renderer(display.GetDevice(),display.GetContext(),display.GetSwapChain());
            mesh.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            auto atlas=std::make_shared<AlphaImage>(); atlas->width=atlas->height=1; atlas->pixels={128}; mesh.texture=atlas;
            renderer.render({mesh});
            uint32_t pixel=renderer.readPixel(32,32);
            nearByte(pixel,0,128); nearByte(pixel,1,64); nearByte(pixel,2,32); nearByte(pixel,3,255);
            require(renderer.readPixel(1,1)==0xFF000000,"Triangle incorrectly covered outside pixel");
            renderer.copyToDisplay(); uint32_t displayPixel=0;
            require(display.ReadbackCenterPixel(displayPixel) && displayPixel==pixel,"Native display copy changed drawn pixel");
            mesh.projection[12]=3; // Row-vector translation moves the triangle beyond clip space.
            renderer.render({mesh}); require(renderer.readPixel(32,32)==0xFF000000,"Projection constant ignored or transposed");
            mesh.projection[12]=0;
            auto transparent=std::make_shared<AlphaImage>(); transparent->width=transparent->height=1; transparent->pixels={0};
            mesh.texture=transparent; renderer.render({mesh});
            require(renderer.readPixel(32,32)==0xFF000000,"Zero-alpha original texture produced visible geometry");

            videoMesh.projection=mesh.projection;
            auto movie=std::make_shared<VideoFrame>(); movie->width=movie->height=2;
            movie->luma={81,81,81,81}; movie->chroma={240,90}; videoMesh.video=movie;
            renderer.render({videoMesh}); pixel=renderer.readPixel(32,32);
            // BE A8L8 stores V before U. Original ARB uses UV-0.5 and alpha zero.
            nearByte(pixel,0,254); nearByte(pixel,1,0); nearByte(pixel,2,0); nearByte(pixel,3,0);
            movie=std::make_shared<VideoFrame>(*movie); movie->chroma={90,240}; videoMesh.video=movie;
            renderer.render({videoMesh}); pixel=renderer.readPixel(32,32);
            nearByte(pixel,0,16); nearByte(pixel,1,62); nearByte(pixel,2,255); nearByte(pixel,3,0);
            movie=std::make_shared<VideoFrame>(*movie); movie->luma={235,235,235,235}; movie->chroma={128,128}; videoMesh.video=movie;
            renderer.render({videoMesh}); pixel=renderer.readPixel(32,32);
            nearByte(pixel,0,255); nearByte(pixel,1,254); nearByte(pixel,2,255); nearByte(pixel,3,0);
            testPreviewVideoUploads(renderer,display,videoMesh);
            mesh.texture=atlas; renderer.render({mesh}); pixel=renderer.readPixel(32,32);
            nearByte(pixel,0,128); nearByte(pixel,1,64); nearByte(pixel,2,32); nearByte(pixel,3,255);
            testColorGpu(base,renderer,display,mesh);
            testPreviewRenderContinuation(renderer,display);
            testPreviewBridge(owner,ctx,renderer);
            testPromptIconPreview(renderer,"framerate-stability-20260909/prompts-preview.bmp");
            testPreviewOutputResize(renderer,display);
            renderer.render({videoMesh}); nearByte(renderer.readPixel(32,32),3,0);
            renderer.render({mesh}); pixel=renderer.readPixel(32,32);
            nearByte(pixel,0,128); nearByte(pixel,1,64); nearByte(pixel,2,32); nearByte(pixel,3,255);
        }
        testPreviewClearState(window);
        DestroyWindow(window); window=nullptr;
        std::puts("EngineMeshContract passed: BE geometry, AOT layout, image bounds, odd-size BC1/BC3, 64 hardware BC texel comparisons, color/alpha blending, YUV and projection.");
        return 0;
    } catch(const std::exception& error) {
        if(window) DestroyWindow(window);
        std::fprintf(stderr,"EngineMeshContract failed: %s\n",error.what()); return 1;
    }
}
