#pragma once
#include "renderer/engine/world_mesh.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace DarkRecomp {
// A conservative union of the original world's constant reads. Range creation
// follows prepareWorldVertexProgramInto and the generated VP.xrg template;
// unknown options or a range outside the bank retain the complete comparison.
// This is only an equality plan. Uploads always retain the complete 256 rows.
struct WorldConstantRanges {
    struct Range {uint16_t first=0,count=0;};
    std::array<Range,32> ranges{};
    uint16_t count=0,rows=0;

    static WorldConstantRanges full() noexcept {
        WorldConstantRanges result;result.ranges[0]={0,256};result.count=1;result.rows=256;return result;
    }
    bool equal(const std::array<Native::EngineVector,256>& uploaded,
               const std::array<Native::EngineVector,256>& requested) const noexcept {
        for(unsigned i=0;i<count;++i) {
            const auto range=ranges[i];
            if(std::memcmp(uploaded.data()+range.first,requested.data()+range.first,
                           size_t(range.count)*sizeof(Native::EngineVector)))return false;
        }
        return true;
    }
};

inline WorldConstantRanges worldConstantRanges(const Native::WorldVertexOptions& options,
    const std::array<std::array<uint32_t,4>,9>& references) noexcept {
    WorldConstantRanges result;
    auto take=[&](uint32_t first,unsigned count) {
        if(first>=256 || count>256-first || result.count==result.ranges.size())return false;
        result.ranges[result.count++]={uint16_t(first),uint16_t(count)};return true;
    };
    if(options.weights>8 || !take(0,4) || !take(7,2) || !take(references[0][2],1) ||
       (options.positionConversion && !take(references[0][1],2)) ||
       (options.weights && !take(references[0][0],156)))return WorldConstantRanges::full();
    for(unsigned stage=0;stage<8;++stage) {
        const unsigned mode=options.modes[stage];
        switch(mode) {
            case 0:case 1:case 4:case 7:case 8:case 9:case 10:case 11:
            case 13:case 16:case 17:case 18:case 20:case 22:case 23:case 24:break;
            default:return WorldConstantRanges::full();
        }
        // Unsupported stage combinations never use a partially known plan,
        // including callers constructing shader options directly in tests.
        if(((mode==7 || mode==13 || mode==22) && stage!=0) || (mode==17 && stage!=5) ||
           (mode==10 && stage!=1) || (mode==11 && stage!=3) || (mode==23 && stage!=4) ||
           (mode==24 && stage!=5) || ((mode==9 || mode==16) && (stage<3 || stage>5)) ||
           (mode==18 && ((stage!=1 && stage!=5) || options.modes[stage+1]!=4 ||
                        options.modes[stage+2]!=4 || options.matrices[stage])))return WorldConstantRanges::full();
        const auto& reference=references[stage+1];
        const bool input=mode==0 || mode==7 || mode==13 || mode==22 ||
            (options.tangents && (stage==2 || stage==3));
        if((input && options.conversions[stage] && !take(reference[0],2)) ||
           (mode!=4 && mode!=7 && options.matrices[stage] && !take(reference[1],4)))return WorldConstantRanges::full();
        unsigned parameterRows=0;
        switch(mode) {
            case 1:parameterRows=4;break;
            case 10:parameterRows=3;break;
            case 18:parameterRows=stage==1?8:3;break;
            case 17:parameterRows=6;break;
            case 22:case 16:parameterRows=2;break;
            case 7:case 9:case 20:parameterRows=1;break;
        }
        if((parameterRows && !take(reference[2],parameterRows)) ||
           (mode==13 && !take(4,2)))return WorldConstantRanges::full();
    }
    std::sort(result.ranges.begin(),result.ranges.begin()+result.count,
              [](auto left,auto right){return left.first<right.first;});
    unsigned merged=0;
    for(unsigned i=0;i<result.count;++i) {
        const auto next=result.ranges[i];
        if(merged && next.first<=result.ranges[merged-1].first+result.ranges[merged-1].count) {
            auto& previous=result.ranges[merged-1];
            previous.count=uint16_t((std::max)(unsigned(previous.first+previous.count),unsigned(next.first+next.count))-previous.first);
        } else result.ranges[merged++]=next;
    }
    result.count=uint16_t(merged);
    for(unsigned i=0;i<merged;++i)result.rows+=result.ranges[i].count;
    return result;
}
}
