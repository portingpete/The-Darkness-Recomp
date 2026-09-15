#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace DarkRecomp {
// Reuse storage only after the renderer releases the immutable CPU snapshot.
// WRITE_DISCARD lets queued GPU work retain its prior contents independently,
// but renaming storage the GPU is still reading stalls the mapping thread
// (multi-ms worst singles were measured). Retired buffers are therefore gated
// on GPU progress via event queries: a buffer last USED in frame U is
// rehanded only after the marker ended at the following beginFrame completes.
// Stale entries evicted long after use are immediately reusable; only
// genuinely fresh storage waits. Polling never blocks (DONOTFLUSH); a removed device retires everything so
// the real error surfaces at the next Map/Create instead of hanging here.
// Without any beginFrame call every entry stays reusable, preserving the
// original single-threaded behavior exactly.
class TransientGeometryBuffers {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Entry {Ptr<ID3D11Buffer> buffer;UINT capacity,bind;uint64_t returned;uint64_t usedSeq;};
    struct Fence {uint64_t seq;Ptr<ID3D11Query> query;};
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    std::vector<Entry> free_;
    std::deque<Fence> fences_;
    std::vector<Ptr<ID3D11Query>> queryPool_;
    std::unordered_map<ID3D11Buffer*,uint64_t> useSeq_;
    uint64_t currentSeq_=0,completedSeq_=0;
    bool framed_=false;
    size_t bytes_=0,byteLimit_,entryLimit_;
    uint64_t created_=0,reused_=0,returned_=0,evicted_=0,fenceDeferred_=0;
    static void check(HRESULT result) {
        if(FAILED(result))throw std::runtime_error("Transient geometry buffer upload failed: "+std::to_string(result));
    }
public:
    static constexpr size_t kByteLimit=64*1024*1024,kEntryLimit=256;
    TransientGeometryBuffers(ID3D11Device* device,ID3D11DeviceContext* context,
                             size_t byteLimit=kByteLimit,size_t entryLimit=kEntryLimit)
        :device_(device),context_(context),byteLimit_((std::min)(byteLimit,kByteLimit)),
         entryLimit_((std::min)(entryLimit,kEntryLimit)) {
        if(!device || !context)throw std::invalid_argument("Missing transient geometry device/context");
        free_.reserve(entryLimit_);
    }
    // Returns the actual GPU allocation size, which can exceed the draw's data.
    UINT upload(const void* data,UINT bytes,UINT bind,Ptr<ID3D11Buffer>& result) {
        if(!data || !bytes || result || (bind!=D3D11_BIND_VERTEX_BUFFER && bind!=D3D11_BIND_INDEX_BUFFER))
            throw std::invalid_argument("Invalid transient geometry upload");
        pollCompleted();
        size_t best=free_.size();
        bool fittingIncomplete=false;
        for(size_t i=0;i<free_.size();++i) {
            const auto& e=free_[i];
            if(e.bind==bind && e.capacity>=bytes && size_t(e.capacity)<=size_t(bytes)*2) {
                // A buffer last used in frame U is safe only after the marker
                // ended at the following beginFrame completes.
                if(framed_ && e.usedSeq+1>completedSeq_) {fittingIncomplete=true;continue;}
                if(best==free_.size() || e.capacity<free_[best].capacity)best=i;
            }
        }
        UINT capacity=bytes;
        Ptr<ID3D11Buffer> buffer;
        if(best!=free_.size()) {
            capacity=free_[best].capacity;buffer=std::move(free_[best].buffer);bytes_-=capacity;
            if(best+1!=free_.size())free_[best]=std::move(free_.back());
            free_.pop_back();++reused_;
        } else {
            if (fittingIncomplete) ++fenceDeferred_;
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.BindFlags=bind;
            desc.Usage=D3D11_USAGE_DYNAMIC;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            check(device_->CreateBuffer(&desc,nullptr,&buffer));++created_;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
        std::memcpy(mapped.pData,data,bytes);context_->Unmap(buffer.Get(),0);
        result=std::move(buffer);useSeq_[result.Get()]=currentSeq_;return capacity;
    }
    void recycle(Ptr<ID3D11Buffer>& buffer,UINT capacity,UINT bind) {
        if(!buffer || !capacity)return;
        if(!entryLimit_ || capacity>byteLimit_) {useSeq_.erase(buffer.Get());buffer.Reset();return;}
        // Prefer the current workload over unused storage from an older scene.
        // Dropping every incoming buffer when full permanently pinned the
        // reserve's initial sizes/types, forcing fresh allocations each frame.
        while(free_.size()>=entryLimit_ || capacity>byteLimit_-bytes_) {
            auto oldest=std::min_element(free_.begin(),free_.end(),[](const auto& a,const auto& b){return a.returned<b.returned;});
            bytes_-=oldest->capacity;
            if(oldest!=free_.end()-1)*oldest=std::move(free_.back());
            free_.pop_back();++evicted_;
        }
        uint64_t used=currentSeq_;
        if(auto found=useSeq_.find(buffer.Get());found!=useSeq_.end()) {used=found->second;useSeq_.erase(found);}
        bytes_+=capacity;free_.push_back({std::move(buffer),capacity,bind,++returned_,used});
    }
    // Marks a GPU progress point after all previously submitted work. Called
    // once per render frame; render thread only, like every other method.
    void beginFrame() {
        framed_=true;++currentSeq_;
        Ptr<ID3D11Query> query;
        if(!queryPool_.empty()) {query=std::move(queryPool_.back());queryPool_.pop_back();}
        else {
            D3D11_QUERY_DESC desc{};desc.Query=D3D11_QUERY_EVENT;desc.MiscFlags=0;
            check(device_->CreateQuery(&desc,&query));
        }
        context_->End(query.Get());
        fences_.push_back({currentSeq_,std::move(query)});
    }
    void pollCompleted() {
        while(!fences_.empty()) {
            BOOL done=FALSE;
            const HRESULT status=context_->GetData(fences_.front().query.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(status==S_FALSE)break;
            // Complete, or device lost (downstream calls surface that error):
            // retire the marker either way so polling can never hang here.
            completedSeq_=fences_.front().seq;
            queryPool_.push_back(std::move(fences_.front().query));
            fences_.pop_front();
        }
    }
    size_t retainedBytes() const {return bytes_;}
    size_t retainedEntries() const {return free_.size();}
    uint64_t created() const {return created_;}
    uint64_t reused() const {return reused_;}
    uint64_t evicted() const {return evicted_;}
    uint64_t fenceDeferred() const {return fenceDeferred_;}
};
}
