#include "renderer/d3d11/transient_geometry_buffers.h"
#include <d3d11sdklayers.h>
#include <array>
#include <cstdio>
#include <string_view>
using Microsoft::WRL::ComPtr;
using DarkRecomp::TransientGeometryBuffers;
static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
static void check(HRESULT result) {require(SUCCEEDED(result),"D3D11 operation failed");}
int main(int argc,char** argv) {try {
    const bool warp=argc>1 && std::string_view(argv[1])=="--warp";
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11InfoQueue> debug;
    check(D3D11CreateDevice(nullptr,warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    check(device.As(&debug));
    TransientGeometryBuffers pool(device.Get(),context.Get());
    constexpr unsigned copies=96,slot=2048;
    D3D11_BUFFER_DESC staging{};staging.ByteWidth=copies*slot;staging.Usage=D3D11_USAGE_STAGING;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> readback;check(device->CreateBuffer(&staging,nullptr,&readback));
    std::array<std::array<uint8_t,slot>,copies> expected{};
    std::array<UINT,copies> sizes{};
    for(unsigned i=0;i<copies;++i) {
        const UINT bytes=sizes[i]=256*(1+i%6),bind=(i&1)?D3D11_BIND_INDEX_BUFFER:D3D11_BIND_VERTEX_BUFFER;
        for(UINT j=0;j<bytes;++j)expected[i][j]=uint8_t(i*37+j*13);
        ComPtr<ID3D11Buffer> buffer;
        UINT capacity=pool.upload(expected[i].data(),bytes,bind,buffer);
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        require(desc.ByteWidth==capacity && capacity>=bytes && capacity<=2*bytes,"Invalid reuse capacity");
        require(desc.BindFlags==bind && desc.Usage==D3D11_USAGE_DYNAMIC && desc.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE,"Incompatible buffer reused");
        const D3D11_BOX box{0,0,0,bytes,1,1};
        context->CopySubresourceRegion(readback.Get(),0,i*slot,0,0,buffer.Get(),0,&box);
        // Recycle immediately, before completing the GPU's prior copies. Each
        // queued copy must still see its own old contents after later discards.
        pool.recycle(buffer,capacity,bind);require(!buffer,"Pool retained caller ownership");
    }
    require(pool.reused()>80 && pool.created()<16,"Steady buffers are still being recreated");
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    for(unsigned i=0;i<copies;++i)
        require(!std::memcmp(static_cast<const uint8_t*>(mapped.pData)+i*slot,expected[i].data(),sizes[i]),"Queued GPU copy observed overwritten geometry");
    context->Unmap(readback.Get(),0);
    std::array<uint8_t,512> bytes{};
    auto bounds=[&](size_t byteLimit,size_t entryLimit,size_t expectedCount) {
        TransientGeometryBuffers bounded(device.Get(),context.Get(),byteLimit,entryLimit);
        std::array<ComPtr<ID3D11Buffer>,4> held;std::array<UINT,4> capacities{};
        for(unsigned i=0;i<held.size();++i)capacities[i]=bounded.upload(bytes.data(),UINT(bytes.size()),D3D11_BIND_VERTEX_BUFFER,held[i]);
        for(unsigned i=0;i<held.size();++i)bounded.recycle(held[i],capacities[i],D3D11_BIND_VERTEX_BUFFER);
        require(bounded.retainedEntries()==expectedCount && bounded.retainedBytes()==expectedCount*bytes.size(),"Pool exceeded or miscounted its bounds");
        for(const auto& buffer:held)require(!buffer,"Overflow retained caller buffer");
    };
    bounds(1024,4,2);bounds(4096,1,1);bounds(0,0,0);
    auto changingWorkload=[&](size_t byteLimit,size_t entryLimit) {
        TransientGeometryBuffers changing(device.Get(),context.Get(),byteLimit,entryLimit);
        std::array<ComPtr<ID3D11Buffer>,2> prior;std::array<UINT,2> capacities{};
        for(unsigned i=0;i<prior.size();++i)capacities[i]=changing.upload(bytes.data(),512,D3D11_BIND_VERTEX_BUFFER,prior[i]);
        for(unsigned i=0;i<prior.size();++i)changing.recycle(prior[i],capacities[i],D3D11_BIND_VERTEX_BUFFER);
        // Fill the reserve with the prior scene's buffers, then change both
        // binding type and size. New working buffers must displace unused ones.
        for(unsigned i=0;i<5;++i) {
            ComPtr<ID3D11Buffer> buffer;UINT capacity=changing.upload(bytes.data(),256,D3D11_BIND_INDEX_BUFFER,buffer);
            changing.recycle(buffer,capacity,D3D11_BIND_INDEX_BUFFER);
        }
        require(changing.created()==3 && changing.reused()==4,"Full reserve permanently rejects the new working set");
        require(changing.evicted()==1,"Replacement did not evict exactly one unused buffer");
        require(changing.retainedEntries()<=entryLimit && changing.retainedBytes()<=byteLimit,"Working-set replacement exceeded limits");
    };
    changingWorkload(4096,2); // Entry limit.
    changingWorkload(1024,8); // Byte limit.
    {
        TransientGeometryBuffers changing(device.Get(),context.Get(),1024,8);
        std::array<ComPtr<ID3D11Buffer>,4> prior;std::array<UINT,4> capacities{};
        for(unsigned i=0;i<prior.size();++i)capacities[i]=changing.upload(bytes.data(),256,D3D11_BIND_VERTEX_BUFFER,prior[i]);
        for(unsigned i=0;i<prior.size();++i)changing.recycle(prior[i],capacities[i],D3D11_BIND_VERTEX_BUFFER);
        std::array<uint8_t,2048> large{};ComPtr<ID3D11Buffer> buffer;
        UINT capacity=changing.upload(large.data(),768,D3D11_BIND_INDEX_BUFFER,buffer);
        changing.recycle(buffer,capacity,D3D11_BIND_INDEX_BUFFER);
        require(changing.retainedBytes()==1024 && changing.retainedEntries()==2,"Replacement did not make enough byte-budget space");
        capacity=changing.upload(large.data(),2048,D3D11_BIND_INDEX_BUFFER,buffer);
        changing.recycle(buffer,capacity,D3D11_BIND_INDEX_BUFFER);
        require(changing.retainedBytes()==1024 && changing.retainedEntries()==2,"Oversized return evicted reusable buffers");
        const auto created=changing.created();
        capacity=changing.upload(large.data(),768,D3D11_BIND_INDEX_BUFFER,buffer);
        require(changing.created()==created,"Oversized return poisoned the reserve");
        changing.recycle(buffer,capacity,D3D11_BIND_INDEX_BUFFER);
    }
    for(const auto [size,bind]:{std::pair{UINT(0),UINT(D3D11_BIND_VERTEX_BUFFER)},std::pair{UINT(16),UINT(D3D11_BIND_CONSTANT_BUFFER)}}) {
        ComPtr<ID3D11Buffer> buffer;bool rejected=false;
        try {pool.upload(bytes.data(),size,bind,buffer);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected && !buffer,"Invalid upload was accepted");
    }
    {
        // Fence-gated retirement: a recycled buffer must not be rehanded while
        // its frame may still execute, then must be reused once proven done.
        TransientGeometryBuffers framed(device.Get(),context.Get());
        std::array<uint8_t,256> payload{};for(unsigned i=0;i<payload.size();++i)payload[i]=uint8_t(i*3+1);
        ComPtr<ID3D11Buffer> first;const UINT firstCapacity=framed.upload(payload.data(),UINT(payload.size()),D3D11_BIND_VERTEX_BUFFER,first);
        ID3D11Buffer* firstRaw=first.Get();
        // Queue enough GPU copy work (~48MB) that the retirement marker below
        // cannot already be complete when the next upload runs microseconds later.
        D3D11_BUFFER_DESC scratch{};scratch.ByteWidth=4*1024*1024;scratch.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        ComPtr<ID3D11Buffer> scratchA,scratchB;
        check(device->CreateBuffer(&scratch,nullptr,&scratchA));
        check(device->CreateBuffer(&scratch,nullptr,&scratchB));
        for(unsigned i=0;i<12;++i)context->CopyResource(scratchB.Get(),scratchA.Get());
        framed.recycle(first,firstCapacity,D3D11_BIND_VERTEX_BUFFER);require(!first,"Framed recycle retained caller ownership");
        framed.beginFrame();
        ComPtr<ID3D11Buffer> second;const UINT secondCapacity=framed.upload(payload.data(),UINT(payload.size()),D3D11_BIND_VERTEX_BUFFER,second);
        ID3D11Buffer* secondRaw=second.Get();
        require(secondRaw!=firstRaw,"Retired buffer rehanded inside its own frame window");
        framed.recycle(second,secondCapacity,D3D11_BIND_VERTEX_BUFFER);
        // Drain the GPU, then advance: reuse must resume once proven complete.
        // Bounded spins stay under the entry limit, so eviction cannot steal
        // either buffer before it is rehanded. Sleep yields to the software
        // rasterizer's worker threads sharing this CPU on WARP.
        context->Flush();
        ComPtr<ID3D11Buffer> third;UINT thirdCapacity=0;bool resumed=false;
        for(unsigned spin=0;spin<120 && !resumed;++spin) {
            Sleep(1);
            if(spin%16==15)context->Flush();
            framed.beginFrame();
            thirdCapacity=framed.upload(payload.data(),UINT(payload.size()),D3D11_BIND_VERTEX_BUFFER,third);
            ID3D11Buffer* raw=third.Get();
            if(raw==firstRaw||raw==secondRaw)resumed=true;
            else framed.recycle(third,thirdCapacity,D3D11_BIND_VERTEX_BUFFER);
        }
        require(resumed,"Fenced buffers never became reusable after GPU completion");
        framed.recycle(third,thirdCapacity,D3D11_BIND_VERTEX_BUFFER);
        require(framed.fenceDeferred()>0,"Fence pressure never recorded");
    }
    for(UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
        SIZE_T size=0;check(debug->GetMessage(i,nullptr,&size));std::vector<uint8_t> storage(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(debug->GetMessage(i,message,&size));
        require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);
    }
    std::printf("TransientGeometryBuffers passed: %s, 96 queued snapshots intact, vertex/index compatibility, bounded reuse, changing workloads, oversized returns and invalid inputs; no D3D11 warnings.\n",warp?"WARP":"hardware");
    return 0;
}catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}}
