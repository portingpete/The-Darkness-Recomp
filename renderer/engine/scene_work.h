#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace DarkRecomp::Native {
// Synchronous parallel ranges over owned data. Callers publish results only
// after success; workers never access live guest state or a D3D context.
class SceneWorkPool {
    using Function = bool (*)(void*,size_t,size_t) noexcept;
    struct Job {
        void* context;
        Function function;
        size_t count,grain,chunks;
        unsigned mxcsr;
        std::atomic<size_t> next{0};
        std::atomic<bool> valid{true};
        std::atomic<unsigned> fpStatus{0};
        std::atomic<uint64_t> assisted{0};
    };
    std::mutex dispatchMutex_,mutex_;
    std::condition_variable ready_,done_;
    std::vector<std::thread> workers_;
    Job* job_=nullptr;
    size_t pending_=0,remaining_=0;
    bool stop_=false;
    std::atomic<uint64_t> batches_{0},assistedChunks_{0};
    static void consume(Job&,bool helper) noexcept;
    void worker() noexcept;
    bool dispatch(size_t,size_t,void*,Function) noexcept;
public:
    explicit SceneWorkPool(unsigned workers) noexcept;
    ~SceneWorkPool();
    SceneWorkPool(const SceneWorkPool&)=delete;
    SceneWorkPool& operator=(const SceneWorkPool&)=delete;
    unsigned workers() const noexcept {return unsigned(workers_.size());}
    uint64_t batches() const noexcept {return batches_.load(std::memory_order_relaxed);}
    uint64_t assistedChunks() const noexcept {return assistedChunks_.load(std::memory_order_relaxed);}
    template<class Work> bool run(size_t count,size_t grain,Work&& work) noexcept {
        return dispatch(count,grain,&work,[](void* raw,size_t first,size_t end) noexcept {
            try {return (*static_cast<std::remove_reference_t<Work>*>(raw))(first,end);}
            catch (...) {return false;}
        });
    }
};
// Configure once before engine startup. Serial is the default; parallel scene
// work remains opt-in until gameplay frame-time measurements show a benefit.
// UINT_MAX selects the available host processors, leaving two for game/display.
void initializeSceneWorkers(unsigned count=0) noexcept;
SceneWorkPool& sceneWorkPool() noexcept;
void printSceneWorkCounters() noexcept;
template<class Work> bool parallelSceneRange(size_t count,size_t grain,Work&& work) noexcept {
    // Small jobs do not even touch the pool's dispatch lock.
    if (!count) return true;
    if (!grain || count/grain<2) {
        try {return work(0,count);} catch (...) {return false;}
    }
    return sceneWorkPool().run(count,grain,std::forward<Work>(work));
}
}
