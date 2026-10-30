#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace DarkRecomp::Native {
// Explicit --frame-timeline diagnostics. Counts describe this logical producer
// frame, not whichever later frame happens to be live when the display polls.
// Normalized diagnostic categories only; no queue representation dependency.
enum class PreviewCommandKind : unsigned {World,Clear,Resolve,Present,Gamma,QueryBegin,QueryEnd,Gui,Count};
struct PreviewFrameDiagnostics {
    uint64_t logicalFrame=0;
    uint32_t producerThreadId=0;
    uint64_t producerCreation100ns=0;
    int64_t producerReplayUs=-1;
    int64_t producerBoundaryNs=0,producerPeriodStartNs=0;
    int64_t producerWallNs=-1,producerCpuNs=-1;
    uint64_t publishWallNs=0,publishWaitNs=0;
    uint64_t commands=0,worldAttempts=0,parts=0;
    std::array<uint64_t,unsigned(PreviewCommandKind::Count)> kinds{};
    uint64_t worldRuns=0,maxWorldRun=0;
    bool mixedProducerThreads=false;
    uint64_t worldCommands() const noexcept {return kinds[unsigned(PreviewCommandKind::World)];}
    uint64_t barriers() const noexcept {return commands-worldCommands();}
};
namespace Detail {
struct PreviewProducerBoundary {
    uint32_t threadId=0;
    uint64_t creation100ns=0,cpu100ns=0;
    int64_t wallNs=0;
    bool cpuValid=false;
};
// Pure reducer, independently testable with synthetic identities/clocks. A
// thread ID alone is insufficient: Windows can reuse it after a thread exits.
class PreviewProducerClock {
    PreviewProducerBoundary previous_{};
public:
    void finish(PreviewFrameDiagnostics& frame,const PreviewProducerBoundary& now) noexcept {
        frame.producerWallNs=frame.producerCpuNs=-1;
        frame.producerThreadId=now.threadId;frame.producerCreation100ns=now.creation100ns;
        frame.producerBoundaryNs=now.wallNs;frame.producerPeriodStartNs=previous_.wallNs;
        const bool wallValid=previous_.wallNs>0 && now.wallNs>=previous_.wallNs;
        if(wallValid)frame.producerWallNs=now.wallNs-previous_.wallNs;
        if(wallValid && !frame.mixedProducerThreads && previous_.cpuValid && now.cpuValid &&
           previous_.threadId==now.threadId && previous_.creation100ns==now.creation100ns &&
           now.cpu100ns>=previous_.cpu100ns && now.cpu100ns-previous_.cpu100ns<=uint64_t((std::numeric_limits<int64_t>::max)())/100)
            frame.producerCpuNs=int64_t((now.cpu100ns-previous_.cpu100ns)*100);
        previous_=now;
    }
};
}
}
