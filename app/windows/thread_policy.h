#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>

// Interactive render/game work should retain HighQoS when the window is
// temporarily covered. The display thread may run above normal priority so
// presentation and input stay responsive while engine workers saturate cores
// during level loads; the engine itself keeps normal priority so guest timing
// is unaffected.
// No processor is suggested or required. With HighQoS, Windows already runs
// these busy threads on performance cores and moves them to the fastest idle
// one. In the busiest village views (about 11,000 draws), an ideal-processor
// hint for the first two performance cores made engine frames 1-5% slower,
// and restricting the engine to the first performance core about 13% slower.
inline void configureGameThread(const char* name,int priority=THREAD_PRIORITY_NORMAL) {
    THREAD_POWER_THROTTLING_STATE power{};
    power.Version=THREAD_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask=THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    const bool highQos=SetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&power,sizeof(power))!=0;
    ULONG bytes=0;GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);
    std::vector<uint8_t> buffer(bytes);
    BYTE highest=0,lowest=255;
    if(bytes && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),bytes,&bytes,GetCurrentProcess(),0)) {
        for(size_t offset=0;offset+sizeof(SYSTEM_CPU_SET_INFORMATION)<=buffer.size();) {
            const auto* info=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data()+offset);
            if(!info->Size || info->Size>buffer.size()-offset)break;
            if(info->Type==CpuSetInformation && (!info->CpuSet.Allocated || info->CpuSet.AllocatedToTargetProcess)) {
                highest=(std::max)(highest,info->CpuSet.EfficiencyClass);lowest=(std::min)(lowest,info->CpuSet.EfficiencyClass);
            }
            offset+=info->Size;
        }
    }
    const bool prioritized=SetThreadPriority(GetCurrentThread(),priority)!=0;
    std::fprintf(stderr,"[Scheduling] %s HighQoS=%u cpuClasses=%u..%u priority=%lu threadPriority=%d prioritized=%u\n",name,unsigned(highQos),unsigned(lowest),unsigned(highest),GetPriorityClass(GetCurrentProcess()),priority,unsigned(prioritized));
}
