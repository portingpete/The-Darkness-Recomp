#pragma once
#include <windows.h>
#include "runtime/native/runtime.h"
#include "renderer/engine/engine_performance.h"
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Optional diagnostic sampler of our own engine and registered worker threads. Resume them before
// allocating, locking, resolving symbols, or writing any log output.
class NativeCpuSampler {
    std::atomic<bool> stop_{false};
    std::atomic<bool> reportRequested_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::unordered_map<uint64_t,unsigned> addresses_;
    using Addresses=std::unordered_map<uint64_t,unsigned>;
    struct OwnedThread {HANDLE value=nullptr;~OwnedThread(){if(value)CloseHandle(value);}} renderThread_;
    bool sampleRender_=false;
    std::atomic<uint64_t> renderEpoch_{0};
    std::array<Addresses,size_t(DarkRecomp::Native::RenderSamplePhase::count)> renderAddresses_;
    std::array<std::atomic<uint64_t>,size_t(DarkRecomp::Native::RenderSamplePhase::count)> renderTotals_{};
    std::unordered_map<std::string,Addresses> workerAddresses_;
    std::unordered_map<std::string,std::pair<double,double>> workerTimes_;
    bool symbols_=false;
    static bool capture(HANDLE thread,uint64_t& address,unsigned* renderPhase=nullptr) {
        CONTEXT state{};state.ContextFlags=CONTEXT_CONTROL;
        if(SuspendThread(thread)==DWORD(-1))return false;
        const bool captured=GetThreadContext(thread,&state)!=0;
        // The display thread is paused here, so this phase belongs to the
        // captured instruction. Only a lock-free atomic load occurs before resume.
        if(renderPhase)*renderPhase=DarkRecomp::Native::renderSamplePhase.load(std::memory_order_relaxed);
        if(ResumeThread(thread)==DWORD(-1)) {
            DWORD code=0;
            if(GetExitCodeThread(thread,&code) && code!=STILL_ACTIVE)return false;
            // An unresolved suspended guest could own arbitrary locks. Avoid
            // logging or unwinding through those locks; fail this diagnostic
            // process instead of silently leaving gameplay suspended.
            fflush(nullptr);
            TerminateProcess(GetCurrentProcess(),3);
            return false;
        }
        if(captured)address=state.Rip;
        return captured;
    }
public:
    NativeCpuSampler(HANDLE thread,bool enabled,bool workers=false,bool renderer=false) {
        if(renderer) {
            sampleRender_=DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),
                &renderThread_.value,0,FALSE,DUPLICATE_SAME_ACCESS)!=0;
            if(!sampleRender_)std::fputs("[CpuRenderSamples] Cannot open the calling render thread; renderer sampling disabled.\n",stderr);
        }
        if(!enabled && !workers && !sampleRender_)return;
        SymSetOptions(SYMOPT_DEFERRED_LOADS|SYMOPT_UNDNAME|SYMOPT_LOAD_LINES|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_IGNORE_NT_SYMPATH);
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        std::wstring directory=executable;directory.resize(directory.find_last_of(L"\\/"));
        symbols_=SymInitializeW(GetCurrentProcess(),directory.c_str(),TRUE)!=0;
        std::fprintf(stderr,"[CpuSamples] symbols=%u executableBase=%p engineTid=%lu workers=%u renderer=%u renderTid=%lu renderOnly=1\n",unsigned(symbols_),GetModuleHandleW(nullptr),GetThreadId(thread),unsigned(workers),unsigned(sampleRender_),GetCurrentThreadId());
        const auto displayTid=GetCurrentThreadId();
        worker_=std::thread([this,thread,enabled,workers,displayTid] {
            const auto timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            if(!timer)return;
            struct Handles {
                std::vector<HANDLE> values;
                void clear(){for(auto handle:values)CloseHandle(handle);values.clear();}
                ~Handles(){clear();}
            } handles;
            struct Target {HANDLE handle;std::string identity;};
            std::vector<Target> targets;
            unsigned refresh=0;size_t selected=0;
            try {
            while(!stop_.load(std::memory_order_relaxed)) {
                // Symbol lookup can take tens of milliseconds. Do it here,
                // with no guest thread suspended, instead of stalling Present
                // every time the window thread asks for a profiling report.
                if(reportRequested_.exchange(false,std::memory_order_relaxed))reportNow();
                if(workers && refresh++%500==0) {
                    // Refresh ownership before suspending any thread. Creation
                    // time keeps recycled Windows thread IDs distinct in logs.
                    targets.clear();handles.clear();
                    handles.values=DarkRecomp::Native::nativeThreadSampleHandles();
                    for(auto handle:handles.values) {
                        const auto tid=GetThreadId(handle);
                        if(!tid || tid==GetThreadId(thread) || tid==displayTid || tid==GetCurrentThreadId())continue;
                        FILETIME created{},exited{},kernel{},user{};
                        if(!GetThreadTimes(handle,&created,&exited,&kernel,&user))continue;
                        auto ticks=[](FILETIME t){return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
                        auto identity="tid="+std::to_string(tid)+" created="+std::to_string(ticks(created))+" ";
                        targets.push_back({handle,identity});
                        std::lock_guard lock(mutex_);
                        workerTimes_[identity]={double(ticks(kernel)+ticks(user))/1e7,double(ticks(user))/1e7};
                    }
                }
                LARGE_INTEGER due{};due.QuadPart=-20000;
                if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))break;
                WaitForSingleObject(timer,INFINITE);
                uint64_t address=0;
                const auto renderEpoch=renderEpoch_.load(std::memory_order_acquire);
                if(sampleRender_ && (renderEpoch&1)) {
                    unsigned phase=0;
                    if(capture(renderThread_.value,address,&phase) && phase<renderAddresses_.size() &&
                       renderEpoch==renderEpoch_.load(std::memory_order_acquire)) {
                        std::lock_guard lock(mutex_);
                        if(renderAddresses_[phase].size()<8192)++renderAddresses_[phase][address];
                        renderTotals_[phase].fetch_add(1,std::memory_order_relaxed);
                    }
                }
                if(enabled && capture(thread,address)) {std::lock_guard lock(mutex_);if(addresses_.size()<8192)++addresses_[address];}
                if(!targets.empty()) {
                    const auto& target=targets[selected++%targets.size()];
                    if(capture(target.handle,address)) {
                        std::lock_guard lock(mutex_);auto& samples=workerAddresses_[target.identity];
                        if(samples.size()<8192)++samples[address];
                    }
                }
            }
            } catch(...) {std::fprintf(stderr,"[CpuSamples] optional worker sampling stopped after diagnostic failure\n");}
            CloseHandle(timer);
        });
    }
    ~NativeCpuSampler() {stop();}
    void beginRender() {if(sampleRender_)renderEpoch_.fetch_add(1,std::memory_order_release);}
    void endRender() {if(sampleRender_)renderEpoch_.fetch_add(1,std::memory_order_release);}
    uint64_t renderSampleCount(DarkRecomp::Native::RenderSamplePhase phase) const {
        return renderTotals_[size_t(phase)].load(std::memory_order_relaxed);
    }
    void stop() {stop_=true;if(worker_.joinable())worker_.join();}
    void report() {
        if(!worker_.joinable())return;
        reportRequested_.store(true,std::memory_order_relaxed);
    }
private:
    void reportNow() {
        std::unordered_map<uint64_t,unsigned> addresses;
        decltype(renderAddresses_) rendering;
        decltype(workerAddresses_) workers;
        decltype(workerTimes_) times;
        {std::lock_guard lock(mutex_);addresses.swap(addresses_);rendering.swap(renderAddresses_);workers.swap(workerAddresses_);times.swap(workerTimes_);}
        if(!addresses.empty())reportGroup(addresses,"","",12,6,16);
        for(size_t phase=0;phase<rendering.size();++phase)if(!rendering[phase].empty()) {
            const std::string identity=std::string("phase=")+DarkRecomp::Native::renderSamplePhaseNames[phase]+" ";
            reportGroup(rendering[phase],"Render",identity.c_str(),8,4,12);
        }
        for(const auto& [identity,time]:times)
            std::fprintf(stderr,"[CpuWorkerTime] %scumulativeCpuSeconds=%.6f cumulativeUserSeconds=%.6f\n",identity.c_str(),time.first,time.second);
        for(const auto& [identity,samples]:workers)reportGroup(samples,"Worker",identity.c_str(),5,2,4);
    }
    void reportGroup(const Addresses& addresses,const char* kind,const char* identity,size_t functionLimit,size_t regionLimit,size_t sourceLimit) {
        std::unordered_map<std::string,unsigned> functions;
        std::unordered_map<std::string,unsigned> sourceLines;
        std::unordered_map<std::string,std::unordered_map<std::string,unsigned>> functionLines;
        unsigned total=0;
        for(const auto& [address,count]:addresses) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
            auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=MAX_SYM_NAME;
            DWORD64 displacement=0;
            if(symbols_ && SymFromAddr(GetCurrentProcess(),address,&displacement,symbol)) {
                if(SymGetModuleBase64(GetCurrentProcess(),address)==reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr))) {
                    functions[symbol->Name]+=count;
                    IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD lineDisplacement=0;
                    if(SymGetLineFromAddr64(GetCurrentProcess(),address,&lineDisplacement,&line) && line.FileName) {
                        // Optimized line mappings identify a source region,
                        // not exclusive time for a single source expression.
                        sourceLines[std::string(line.FileName)+":"+std::to_string(line.LineNumber)+" function="+symbol->Name]+=count;
                        functionLines[symbol->Name][std::string(line.FileName)+":"+std::to_string(line.LineNumber)]+=count;
                    }
                }
                else {
                    // System DLLs may expose only sparse export symbols. Keep
                    // the sampled address and displacement instead of implying
                    // that the nearest exported name is the sampled function.
                    char suffix[96];std::snprintf(suffix,sizeof(suffix),"+0x%llX [address=0x%llX, nearest export]",displacement,address);
                    IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);
                    const std::string prefix=SymGetModuleInfo64(GetCurrentProcess(),address,&module)?std::string(module.ModuleName)+"!":"";
                    functions[prefix+symbol->Name+suffix]+=count;
                }
            }
            else {
                IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);char label[128];
                if(symbols_ && SymGetModuleInfo64(GetCurrentProcess(),address,&module))
                    std::snprintf(label,sizeof(label),"%s+0x%llX [module offset, no symbol]",module.ModuleName,address-module.BaseOfImage);
                else std::snprintf(label,sizeof(label),"0x%llX",address);
                functions[label]+=count;
            }
            total+=count;
        }
        std::vector<std::pair<std::string,unsigned>> sorted(functions.begin(),functions.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second>b.second;});
        std::fprintf(stderr,"[Cpu%sSamples] %stotal=%u (diagnostic sampling enabled)\n",kind,identity,total);
        for(size_t i=0;i<(std::min)(sorted.size(),functionLimit);++i) {
            std::fprintf(stderr,"[Cpu%sHotspot] %ssamples=%u function=%s\n",kind,identity,sorted[i].second,sorted[i].first.c_str());
            // A large function can spread its samples across enough lines to
            // disappear from the global source top list. Retain its busiest
            // regions too; the counts remain diagnostic, nonexclusive samples.
            const auto found=functionLines.find(sorted[i].first);
            if(found==functionLines.end())continue;
            std::vector<std::pair<std::string,unsigned>> regions(found->second.begin(),found->second.end());
            std::sort(regions.begin(),regions.end(),[](const auto& a,const auto& b){return a.second>b.second;});
            for(size_t j=0;j<(std::min)(regions.size(),regionLimit);++j)
                std::fprintf(stderr,"[Cpu%sRegion] %ssamples=%u function=%s location=%s\n",kind,identity,regions[j].second,sorted[i].first.c_str(),regions[j].first.c_str());
        }
        sorted.assign(sourceLines.begin(),sourceLines.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second>b.second;});
        for(size_t i=0;i<(std::min)(sorted.size(),sourceLimit);++i)
            std::fprintf(stderr,"[Cpu%sSource] %ssamples=%u location=%s\n",kind,identity,sorted[i].second,sorted[i].first.c_str());
    }
};
