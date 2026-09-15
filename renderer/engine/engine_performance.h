#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace DarkRecomp::Native {
enum class EnginePhase { stored, immediate, indexed, vertexProgram, descriptor, conversion, textures, palette, snapshot, handoff, sleep,
                         queryXma, queryKernel, queryDispatcher, queryAudio, queryFile, count };
struct EnginePhaseCounters {std::atomic<uint64_t> nanoseconds{}, calls{};};
inline std::array<EnginePhaseCounters,size_t(EnginePhase::count)> enginePhases;
inline bool profileEngineCpu=false; // Set before starting the engine thread.
inline bool sampleRendererCpu=false; // Opt-in; set before any rendering starts.
enum class RenderSamplePhase : unsigned { other,textures,geometry,constants,indices,states,submit,clear,resolve,copy,queries,frame,count };
inline constexpr std::array<const char*,size_t(RenderSamplePhase::count)> renderSamplePhaseNames{
    "other","textures","geometry","constants","indices","states","submit","clear","resolve","copy","queries","frame"};
inline std::atomic<unsigned> renderSamplePhase{unsigned(RenderSamplePhase::other)};
static_assert(std::atomic<unsigned>::is_always_lock_free);
inline void setRenderSamplePhase(RenderSamplePhase phase) {
    if(sampleRendererCpu)renderSamplePhase.store(unsigned(phase),std::memory_order_relaxed);
}
inline std::atomic<uint64_t> renderMemoryFaults{};
inline std::array<std::atomic<unsigned>,256> engineFrameProcessors{};
inline std::atomic<uint64_t> xmaProfileBatchCalls{}, xmaProfileBatchContexts{}, xmaProfileSingleBatches{};
inline void recordXmaProfileBatch(size_t contexts) {
    if(!profileEngineCpu)return;
    xmaProfileBatchCalls.fetch_add(1,std::memory_order_relaxed);
    xmaProfileBatchContexts.fetch_add(contexts,std::memory_order_relaxed);
    if(contexts==1)xmaProfileSingleBatches.fetch_add(1,std::memory_order_relaxed);
}
inline void recordEngineFrameProcessor() {
    if(!profileEngineCpu)return;
    const auto processor=GetCurrentProcessorNumber();
    if(processor<engineFrameProcessors.size())engineFrameProcessors[processor].fetch_add(1,std::memory_order_relaxed);
}
class EngineCpuScope {
    using Clock=std::chrono::steady_clock;
    EnginePhase phase_;
    Clock::time_point start_;
    bool enabled_;
public:
    explicit EngineCpuScope(EnginePhase phase,bool active=true):phase_(phase),enabled_(profileEngineCpu && active) {if(enabled_)start_=Clock::now();}
    ~EngineCpuScope() {
        if(!enabled_)return;
        auto& counter=enginePhases[size_t(phase_)];
        counter.nanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start_).count(),std::memory_order_relaxed);
        counter.calls.fetch_add(1,std::memory_order_relaxed);
    }
};
// Wrap the query expression itself so existing short-circuit validation and
// cache-hit paths stay intact. Disabled profiling performs the original query.
inline SIZE_T engineProfileVirtualQuery(EnginePhase phase,LPCVOID address,
                                       PMEMORY_BASIC_INFORMATION info,SIZE_T bytes) {
    if(!profileEngineCpu)return VirtualQuery(address,info,bytes);
    EngineCpuScope query(phase);
    return VirtualQuery(address,info,bytes);
}
inline void printEngineCpuPerformance() {
    if(!profileEngineCpu)return;
    constexpr const char* names[]{"stored","immediate","indexed","vertexProgram","descriptor","conversion","textures","palette","snapshot","handoff","mainSleep",
                                 "vqXma","vqKernel","vqDispatcher","vqAudio","vqFile"};
    static_assert(std::size(names)==size_t(EnginePhase::count));
    std::fprintf(stderr,"[EngineCPU] inclusiveMs");
    for(size_t i=0;i<enginePhases.size();++i) {
        const auto ns=enginePhases[i].nanoseconds.exchange(0,std::memory_order_relaxed);
        const auto calls=enginePhases[i].calls.exchange(0,std::memory_order_relaxed);
        std::fprintf(stderr," %s=%.2f/%llu",names[i],double(ns)/1e6,calls);
    }
    std::fputc('\n',stderr);
    // These are bridge batches entered after Memory's first ownership check.
    // Requested contexts include an undecoded suffix when a batch fails.
    const auto batches=xmaProfileBatchCalls.exchange(0,std::memory_order_relaxed);
    const auto contexts=xmaProfileBatchContexts.exchange(0,std::memory_order_relaxed);
    const auto single=xmaProfileSingleBatches.exchange(0,std::memory_order_relaxed);
    std::fprintf(stderr,"[XmaBatch] entered=%llu requestedContexts=%llu size1=%llu\n",batches,contexts,single);
    std::fprintf(stderr,"[RenderMemory] caughtFaults=%llu\n",renderMemoryFaults.load(std::memory_order_relaxed));
    std::fprintf(stderr,"[EngineProcessors]");
    for(unsigned i=0;i<engineFrameProcessors.size();++i)
        if(auto frames=engineFrameProcessors[i].exchange(0,std::memory_order_relaxed))std::fprintf(stderr," %u=%u",i,frames);
    std::fputc('\n',stderr);
}
}
