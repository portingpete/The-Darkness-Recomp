#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace DarkRecomp::Native {
struct HostProcessor {WORD group;BYTE number,core,efficiency;};

// Guest processor numbers are synchronization identities, not Windows logical
// processor indices. Prefer different physical cores over SMT siblings. On an
// eight-P-core host, leave the first two cores for the native engine/display
// hints in app/windows/thread_policy.h. Restricted and smaller hosts still map
// every guest identity onto an available processor.
inline std::array<unsigned,6> mapGuestProcessors(std::vector<HostProcessor> cpus,WORD group,DWORD_PTR allowed) {
    std::erase_if(cpus,[&](auto p){return p.group!=group || p.number>=sizeof(DWORD_PTR)*8 || !(allowed&(DWORD_PTR(1)<<p.number));});
    std::sort(cpus.begin(),cpus.end(),[](auto a,auto b){return a.efficiency!=b.efficiency?a.efficiency>b.efficiency:a.number<b.number;});
    std::vector<HostProcessor> physical;
    for(auto cpu:cpus)if(std::none_of(physical.begin(),physical.end(),[&](auto p){return p.core==cpu.core;}))physical.push_back(cpu);
    if(physical.empty())for(unsigned cpu=0;cpu<sizeof(DWORD_PTR)*8;++cpu)
        if(allowed&(DWORD_PTR(1)<<cpu))physical.push_back({group,BYTE(cpu),BYTE(cpu),0});
    std::array<unsigned,6> result;result.fill(sizeof(DWORD_PTR)*8);
    if(physical.empty())return result;
    const auto fast=std::count_if(physical.begin(),physical.end(),[&](auto p){return p.efficiency==physical.front().efficiency;});
    const unsigned start=fast>=8?2:0;
    for(unsigned guest=0;guest<result.size();++guest)result[guest]=physical[(start+guest)%physical.size()].number;
    return result;
}
inline DWORD_PTR guestProcessorMask(uint32_t guest,const std::array<unsigned,6>& map) {
    if(!guest || (guest&~0x3fu))return 0;
    DWORD_PTR result=0;
    for(unsigned n=0;n<map.size();++n)if((guest&(1u<<n)) && map[n]<sizeof(DWORD_PTR)*8)result|=DWORD_PTR(1)<<map[n];
    return result;
}
inline DWORD_PTR nativeGuestAffinity(uint32_t guest,DWORD_PTR allowed,WORD group) {
    ULONG bytes=0;GetSystemCpuSetInformation(nullptr,0,&bytes,GetCurrentProcess(),0);
    std::vector<uint8_t> buffer(bytes);std::vector<HostProcessor> cpus;
    if(bytes && GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),bytes,&bytes,GetCurrentProcess(),0)) {
        for(size_t offset=0;offset+sizeof(SYSTEM_CPU_SET_INFORMATION)<=buffer.size();) {
            const auto* info=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data()+offset);
            if(!info->Size || info->Size>buffer.size()-offset)break;
            if(info->Type==CpuSetInformation && (!info->CpuSet.Allocated || info->CpuSet.AllocatedToTargetProcess)) {
                const auto& p=info->CpuSet;cpus.push_back({p.Group,p.LogicalProcessorIndex,p.CoreIndex,p.EfficiencyClass});
            }
            offset+=info->Size;
        }
    }
    return guestProcessorMask(guest,mapGuestProcessors(std::move(cpus),group,allowed));
}
}
