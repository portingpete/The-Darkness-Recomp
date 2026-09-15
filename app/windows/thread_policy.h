#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>

// Interactive render/game work should retain HighQoS when the window is
// temporarily covered. Ideal-processor hints leave Windows free to migrate it.
// The display thread may run above normal priority so presentation and input
// stay responsive while engine workers saturate cores during level loads; the
// engine itself keeps normal priority so guest timing is unaffected.
inline void configureGameThread(const char* name,unsigned preferredCore,int priority=THREAD_PRIORITY_NORMAL) {
    THREAD_POWER_THROTTLING_STATE power{};
    power.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask=THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    const bool highQos=SetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&power,sizeof(power))!=0;
    ULONG bytes=0;GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);
    std::vector<uint8_t> buffer(bytes);
    struct Core {WORD group;BYTE number,core,efficiency;};
    std::vector<Core> cores;
    BYTE highest=0,lowest=255;
    if(bytes && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),bytes,&bytes,GetCurrentProcess(),0)) {
        for(size_t offset=0;offset+sizeof(SYSTEM_CPU_SET_INFORMATION)<=buffer.size();) {
            const auto* info=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data()+offset);
            if(!info->Size || info->Size>buffer.size()-offset)break;
            if(info->Type==CpuSetInformation && (!info->CpuSet.Allocated || info->CpuSet.AllocatedToTargetProcess)) {
                const auto& cpu=info->CpuSet;
                cores.push_back({cpu.Group,cpu.LogicalProcessorIndex,cpu.CoreIndex,cpu.EfficiencyClass});
                highest=(std::max)(highest,cpu.EfficiencyClass);lowest=(std::min)(lowest,cpu.EfficiencyClass);
            }
            offset+=info->Size;
        }
    }
    std::vector<Core> performance;
    for(const auto& core:cores)if(core.efficiency==highest && std::none_of(performance.begin(),performance.end(),[&](const auto& c){return c.group==core.group && c.core==core.core;}))performance.push_back(core);
    bool ideal=false;
    if(highest>lowest && !performance.empty()) {
        const auto& cpu=performance[preferredCore%performance.size()];
        PROCESSOR_NUMBER target{cpu.group,cpu.number,0};
        ideal=SetThreadIdealProcessorEx(GetCurrentThread(),&target,nullptr)!=0;
    }
    const bool prioritized=SetThreadPriority(GetCurrentThread(),priority)!=0;
    std::fprintf(stderr,"[Scheduling] %s HighQoS=%u performanceCoreHint=%u cpuClasses=%u..%u priority=%lu threadPriority=%d prioritized=%u\n",name,unsigned(highQos),unsigned(ideal),unsigned(lowest),unsigned(highest),GetPriorityClass(GetCurrentProcess()),priority,unsigned(prioritized));
}
