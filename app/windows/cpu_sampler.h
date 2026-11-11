#pragma once
#include <windows.h>
#include "runtime/native/runtime.h"
#include "renderer/engine/engine_performance.h"
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
    // Engine samples outside this executable (system DLLs such as ntdll's
    // SRW lock functions), keyed by {sampled address, first return address
    // into this executable found on the stack}.
    struct PairHash {size_t operator()(const std::pair<uint64_t,uint64_t>& v) const noexcept {return std::hash<uint64_t>{}(v.first*0x9E3779B97F4A7C15ull^v.second);}};
    std::unordered_map<std::pair<uint64_t,uint64_t>,unsigned,PairHash> systemCallers_;
    // The same for display-thread samples outside this executable (D3D runtime, driver, C runtime).
    decltype(systemCallers_) renderSystemCallers_;
    uint64_t imageLo_=0,imageHi_=0;
    bool symbols_=false;
    // Runs while the target is suspended: reads only its committed stack, no
    // allocation or locks. The first value above Rsp that points into this
    // executable is the nearest executable caller of the system code (stale
    // slots can rarely misattribute; diagnostic counts only).
    static uint64_t executableCaller(uint64_t rsp,uint64_t lo,uint64_t hi) noexcept {
        __try {
            const auto* stack=reinterpret_cast<const volatile uint64_t*>(rsp);
            for(unsigned i=0;i<64;++i) {const uint64_t value=stack[i];if(value>=lo && value<hi)return value;}
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    static bool capture(HANDLE thread,uint64_t& address,unsigned* renderPhase=nullptr,
                        uint64_t lo=0,uint64_t hi=0,uint64_t* caller=nullptr) {
        CONTEXT state{};state.ContextFlags=CONTEXT_CONTROL;
        if(SuspendThread(thread)==DWORD(-1))return false;
        const bool captured=GetThreadContext(thread,&state)!=0;
        // The display thread is paused here, so this phase belongs to the
        // captured instruction. Only a lock-free atomic load occurs before resume.
        if(renderPhase)*renderPhase=DarkRecomp::Native::renderSamplePhase.load(std::memory_order_relaxed);
        if(caller)*caller=captured && hi && (state.Rip<lo || state.Rip>=hi)?executableCaller(state.Rsp,lo,hi):0;
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
        {
            const auto* image=reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(image+dos->e_lfanew);
            // Code sections only: data addresses (globals) left on the stack
            // are not callers.
            const auto* section=IMAGE_FIRST_SECTION(nt);
            for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i,++section) {
                if(!(section->Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
                const uint64_t lo=reinterpret_cast<uint64_t>(image)+section->VirtualAddress;
                const uint64_t hi=lo+section->Misc.VirtualSize;
                if(!imageLo_ || lo<imageLo_)imageLo_=lo;
                if(hi>imageHi_)imageHi_=hi;
            }
        }
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
                    uint64_t renderCaller=0;
                    if(capture(renderThread_.value,address,&phase,imageLo_,imageHi_,&renderCaller) && phase<renderAddresses_.size() &&
                       renderEpoch==renderEpoch_.load(std::memory_order_acquire)) {
                        std::lock_guard lock(mutex_);
                        if(renderAddresses_[phase].size()<8192)++renderAddresses_[phase][address];
                        if(renderCaller && renderSystemCallers_.size()<8192)++renderSystemCallers_[{address,renderCaller}];
                        renderTotals_[phase].fetch_add(1,std::memory_order_relaxed);
                    }
                }
                uint64_t caller=0;
                if(enabled && capture(thread,address,nullptr,imageLo_,imageHi_,&caller)) {
                    std::lock_guard lock(mutex_);if(addresses_.size()<8192)++addresses_[address];
                    if(caller && systemCallers_.size()<8192)++systemCallers_[{address,caller}];
                }
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
        decltype(systemCallers_) callers,renderCallers;
        {std::lock_guard lock(mutex_);addresses.swap(addresses_);rendering.swap(renderAddresses_);workers.swap(workerAddresses_);times.swap(workerTimes_);callers.swap(systemCallers_);renderCallers.swap(renderSystemCallers_);}
        if(!addresses.empty())reportGroup(addresses,"","",40,6,24,true);
        if(!callers.empty())reportSystemCallers(callers,"");
        if(!renderCallers.empty())reportSystemCallers(renderCallers,"Render");
        for(size_t phase=0;phase<rendering.size();++phase)if(!rendering[phase].empty()) {
            const std::string identity=std::string("phase=")+DarkRecomp::Native::renderSamplePhaseNames[phase]+" ";
            reportGroup(rendering[phase],"Render",identity.c_str(),8,4,12);
        }
        for(const auto& [identity,time]:times)
            std::fprintf(stderr,"[CpuWorkerTime] %scumulativeCpuSeconds=%.6f cumulativeUserSeconds=%.6f\n",identity.c_str(),time.first,time.second);
        for(const auto& [identity,samples]:workers)reportGroup(samples,"Worker",identity.c_str(),5,2,4);
    }
    // Guest address from a recompiled name such as __imp__sub_82868FE8.
    static uint32_t guestAddress(const char* name) {
        const char* at=std::strstr(name,"sub_");
        if(!at)return 0;
        char* end=nullptr;
        const unsigned long value=std::strtoul(at+4,&end,16);
        return end==at+12?uint32_t(value):0;
    }
    // Category of a function from the source file that defines it.
    static const char* sourceCategory(std::string file) {
        std::transform(file.begin(),file.end(),file.begin(),[](unsigned char c){return char(c=='/'?'\\':std::tolower(c));});
        static constexpr const char* folders[][2]{
            {"\\build_native\\generated\\","guest"},{"\\renderer\\engine\\","port:renderer/engine"},
            {"\\renderer\\d3d11\\","port:renderer/d3d11"},{"\\renderer\\","port:renderer/other"},
            {"\\runtime\\","port:runtime"},{"\\app\\","port:app"},{"\\third_party\\","port:third_party"}};
        for(const auto& folder:folders)if(file.find(folder[0])!=std::string::npos)return folder[1];
        return "port:other";
    }
    // Engine samples in system DLLs grouped by system function (nearest
    // export) and the executable source line that called into it.
    void reportSystemCallers(const decltype(systemCallers_)& callers,const char* kind) {
        std::unordered_map<std::string,unsigned> grouped;
        unsigned total=0;
        for(const auto& [key,count]:callers) {
            std::string system="unknown",site="unknown";
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
            auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=MAX_SYM_NAME;
            DWORD64 displacement=0;
            if(symbols_ && SymFromAddr(GetCurrentProcess(),key.first,&displacement,symbol)) {
                IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);
                system=(SymGetModuleInfo64(GetCurrentProcess(),key.first,&module)?std::string(module.ModuleName)+"!":std::string())+symbol->Name;
            }
            std::memset(buffer,0,sizeof(buffer));symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=MAX_SYM_NAME;
            if(symbols_ && SymFromAddr(GetCurrentProcess(),key.second,&displacement,symbol)) {
                site=symbol->Name;
                // Return addresses point after the call; look up the call itself.
                IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD lineDisplacement=0;
                if(SymGetLineFromAddr64(GetCurrentProcess(),key.second-1,&lineDisplacement,&line) && line.FileName)
                    site+=" at "+std::string(line.FileName)+":"+std::to_string(line.LineNumber);
            }
            grouped["system="+system+" caller="+site]+=count;total+=count;
        }
        std::vector<std::pair<std::string,unsigned>> sorted(grouped.begin(),grouped.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second>b.second;});
        std::fprintf(stderr,"[Cpu%sSystemCallers] total=%u\n",kind,total);
        for(size_t i=0;i<(std::min)(sorted.size(),size_t(*kind?40:24));++i)
            std::fprintf(stderr,"[Cpu%sSystemCaller] samples=%u %s\n",kind,sorted[i].second,sorted[i].first.c_str());
    }
    void reportGroup(const Addresses& addresses,const char* kind,const char* identity,size_t functionLimit,size_t regionLimit,size_t sourceLimit,bool breakdown=false) {
        // With breakdown, every sample (not only the top lists) is attributed:
        // recompiled guest code by 64 KiB guest-address block, port code by the
        // source folder defining the sampled function, other modules by name.
        // Inlined callees count toward the function they were inlined into.
        std::map<std::string,unsigned> categories;
        std::map<uint32_t,unsigned> guestBlocks;
        std::unordered_map<std::string,unsigned> guestFunctions;
        std::unordered_map<std::string,std::string> definitionCategory;
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
                    if(breakdown) {
                        auto found=definitionCategory.find(symbol->Name);
                        if(found==definitionCategory.end()) {
                            IMAGEHLP_LINE64 start{};start.SizeOfStruct=sizeof(start);DWORD startDisplacement=0;
                            const char* category=SymGetLineFromAddr64(GetCurrentProcess(),symbol->Address,&startDisplacement,&start) && start.FileName ?
                                sourceCategory(start.FileName) : std::strncmp(symbol->Name,"__imp__sub_",11)==0 ? "guest" : "port:no-line";
                            found=definitionCategory.emplace(symbol->Name,category).first;
                        }
                        categories[found->second]+=count;
                        if(found->second=="guest") {
                            if(const auto guest=guestAddress(symbol->Name))guestBlocks[guest>>16]+=count;
                            guestFunctions[symbol->Name]+=count;
                        }
                    }
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
                    if(breakdown)categories[prefix.empty()?std::string("system:unknown"):"system:"+prefix.substr(0,prefix.size()-1)]+=count;
                }
            }
            else {
                IMAGEHLP_MODULE64 module{};module.SizeOfStruct=sizeof(module);char label[128];
                const bool haveModule=symbols_ && SymGetModuleInfo64(GetCurrentProcess(),address,&module);
                if(haveModule)
                    std::snprintf(label,sizeof(label),"%s+0x%llX [module offset, no symbol]",module.ModuleName,address-module.BaseOfImage);
                else std::snprintf(label,sizeof(label),"0x%llX",address);
                functions[label]+=count;
                if(breakdown)categories[!haveModule?std::string("unresolved"):
                    module.BaseOfImage==reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr))?std::string("port:no-symbol"):
                    "system:"+std::string(module.ModuleName)]+=count;
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
        if(!breakdown || !total)return;
        std::vector<std::pair<std::string,unsigned>> ordered(categories.begin(),categories.end());
        std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.second>b.second;});
        std::string line="[Cpu"+std::string(kind)+"Breakdown] "+identity+"total="+std::to_string(total);
        for(const auto& [name,count]:ordered)line+=" "+name+"="+std::to_string(count);
        std::fprintf(stderr,"%s\n",line.c_str());
        // 0x8285-0x8287 holds the Xbox video/Direct3D runtime (its Vd* import
        // callers); the game's own code lies below it.
        unsigned runtime=0,below=0,above=0;
        line="[Cpu"+std::string(kind)+"GuestBlocks] "+identity+"unit=64KiB";
        for(const auto& [block,count]:guestBlocks) {
            char item[32];std::snprintf(item,sizeof(item)," %04X=%u",block,count);line+=item;
            (block<0x8285?below:block<=0x8287?runtime:above)+=count;
        }
        std::fprintf(stderr,"%s\n",line.c_str());
        std::fprintf(stderr,"[Cpu%sGuestRegions] %sgameBelow8285=%u xboxRuntime8285to8287=%u above8287=%u\n",kind,identity,below,runtime,above);
        sorted.assign(guestFunctions.begin(),guestFunctions.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second>b.second;});
        for(size_t i=0;i<(std::min)(sorted.size(),size_t(40));++i)
            std::fprintf(stderr,"[Cpu%sGuestTop] %ssamples=%u function=%s\n",kind,identity,sorted[i].second,sorted[i].first.c_str());
    }
};
