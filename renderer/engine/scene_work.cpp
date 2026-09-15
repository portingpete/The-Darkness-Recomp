#include "scene_work.h"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <new>
#include <xmmintrin.h>

namespace DarkRecomp::Native {
namespace {
std::atomic<unsigned> configuredWorkers{0};
thread_local unsigned sceneDispatchDepth=0;
struct SceneDispatchScope {
    SceneDispatchScope() noexcept {++sceneDispatchDepth;}
    ~SceneDispatchScope() {--sceneDispatchDepth;}
};
unsigned availableWorkers() noexcept {
    // Honor restricted process affinity. No worker is pinned to a guest CPU
    // identity; Windows remains free to place it on any allowed host core.
    DWORD_PTR allowed=0,system=0;
    unsigned count=std::thread::hardware_concurrency();
    if(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system) && allowed)
        count=std::popcount(allowed);
    return count>2?(std::min)(count-2,256u):0;
}
}
SceneWorkPool::SceneWorkPool(unsigned count) noexcept {
    try {
        workers_.reserve(count);
        for(unsigned i=0;i<count;++i)workers_.emplace_back([this]{worker();});
    } catch (...) {
        // A partially created pool is usable. Resource exhaustion must not
        // prevent a level from loading; zero workers executes synchronously.
    }
}
SceneWorkPool::~SceneWorkPool() {
    std::lock_guard dispatch(dispatchMutex_);
    {std::lock_guard lock(mutex_);stop_=true;}
    ready_.notify_all();
    for(auto& thread:workers_)thread.join();
}
void SceneWorkPool::consume(Job& job,bool helper) noexcept {
    SceneDispatchScope scope;
    const unsigned previous=_mm_getcsr();
    if(helper)_mm_setcsr(job.mxcsr);
    uint64_t completed=0;
    while(job.valid.load(std::memory_order_relaxed)) {
        const size_t chunk=job.next.fetch_add(1,std::memory_order_relaxed);
        if(chunk>=job.chunks)break;
        const size_t first=chunk*job.grain;
        const size_t end=first+(std::min)(job.grain,job.count-first);
        if(!job.function(job.context,first,end))job.valid.store(false,std::memory_order_relaxed);
        ++completed;
    }
    job.fpStatus.fetch_or(_mm_getcsr()&0x3fu,std::memory_order_relaxed);
    if(helper) {
        job.assisted.fetch_add(completed,std::memory_order_relaxed);
        _mm_setcsr(previous);
    }
}
void SceneWorkPool::worker() noexcept {
    SetThreadDescription(GetCurrentThread(),L"DarkRecomp scene worker");
    for(;;) {
        Job* selected;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock,[&]{return stop_ || pending_;});
            if(stop_)return;
            --pending_;selected=job_;
        }
        consume(*selected,true);
        {
            std::lock_guard lock(mutex_);
            if(--remaining_==0)done_.notify_one();
        }
    }
}
bool SceneWorkPool::dispatch(size_t count,size_t grain,void* context,Function function) noexcept {
    if(!count)return true;
    // Disabled scene workers must not add a dispatch lock or TLS bookkeeping
    // to the serial path used by normal gameplay.
    if(!grain || count/grain<2 || workers_.empty())return function(context,0,count);
    // Do not try_lock a nonrecursive mutex already owned by this thread.
    // Nested jobs are small synchronous continuations, including on helpers.
    if(sceneDispatchDepth)return function(context,0,count);
    SceneDispatchScope scope;
    // Concurrent engine/render calls and recursive worker calls execute
    // locally instead of waiting for another batch or deadlocking on it.
    std::unique_lock dispatch(dispatchMutex_,std::try_to_lock);
    if(!dispatch.owns_lock())
        return function(context,0,count);
    Job job{context,function,count,grain,1+(count-1)/grain,_mm_getcsr()};
    const size_t helpers=(std::min)(workers_.size(),job.chunks-1);
    {
        std::lock_guard lock(mutex_);
        job_=&job;pending_=remaining_=helpers;
    }
    for(size_t i=0;i<helpers;++i)ready_.notify_one();
    consume(job,false);
    {
        std::unique_lock lock(mutex_);
        done_.wait(lock,[&]{return remaining_==0;});
        job_=nullptr;
    }
    _mm_setcsr(job.mxcsr|job.fpStatus.load(std::memory_order_relaxed));
    batches_.fetch_add(1,std::memory_order_relaxed);
    assistedChunks_.fetch_add(job.assisted.load(std::memory_order_relaxed),std::memory_order_relaxed);
    return job.valid.load(std::memory_order_relaxed);
}
SceneWorkPool& sceneWorkPool() noexcept {
    // The game exits with guest threads still owning its address space. Keep
    // this shared pool alive through cross-translation-unit static teardown.
    alignas(SceneWorkPool) static std::byte storage[sizeof(SceneWorkPool)];
    static SceneWorkPool* pool=[] {
        const auto requested=configuredWorkers.load(std::memory_order_relaxed);
        const auto available=availableWorkers();
        return ::new(static_cast<void*>(storage)) SceneWorkPool(requested==UINT32_MAX?available:(std::min)(requested,available));
    }();
    return *pool;
}
void initializeSceneWorkers(unsigned count) noexcept {
    configuredWorkers.store(count,std::memory_order_relaxed);
    std::fprintf(stderr,"[SceneWorkers] threads=%u smallJobs=inline ownedDataOnly=1\n",sceneWorkPool().workers());
}
void printSceneWorkCounters() noexcept {
    auto& pool=sceneWorkPool();
    std::fprintf(stderr,"[SceneWork] workers=%u batches=%llu helperChunks=%llu\n",
        pool.workers(),pool.batches(),pool.assistedChunks());
}
}
