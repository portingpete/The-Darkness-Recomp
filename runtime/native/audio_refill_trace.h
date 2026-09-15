#pragma once
#include "audio_resampler_trace.h"
#include <algorithm>
#include <cstring>

namespace DarkRecomp::Native {
// One fixed-size record per entire refill pass. No per-sample snapshots,
// memory queries, allocation or file writes are added to the refill worker.
// Payload is 22 host little-endian u64 fields, tagged 827D9188.
struct AudioRefillStats {
    uint64_t end{}, streams{}, streamTicks{}, maxStreamTicks{}, batches{},
        submitTicks{}, decodeTicks{}, producedFrames{}, contexts{}, emptyInputs{},
        fullRings{}, maxDecodeTicks{}, packets{}, quotaFrames{}, roomFrames{},
        guestFrames{}, unchangedStreams{}, threadCycles{}, reserved[4]{};
};
static_assert(sizeof(AudioRefillStats) == 176);
inline thread_local AudioRefillStats* activeAudioRefill = nullptr;
inline uint64_t audioRefillClock() noexcept {
    LARGE_INTEGER time{};
    QueryPerformanceCounter(&time);
    return uint64_t(time.QuadPart);
}
class AudioRefillTimer {
    uint64_t *total_, *maximum_, start_;
public:
    explicit AudioRefillTimer(uint64_t* total, uint64_t* maximum = nullptr)
        : total_(total), maximum_(maximum), start_(total ? audioRefillClock() : 0) {}
    ~AudioRefillTimer() {
        if (!total_) return;
        const auto ticks = audioRefillClock() - start_;
        *total_ += ticks;
        if (maximum_) *maximum_ = (std::max)(*maximum_, ticks);
    }
};
class AudioRefillPass {
    AudioResamplerTrace::Call call_;
    AudioRefillStats stats_{};
    AudioRefillStats* previous_ = activeAudioRefill;
    ULONG64 cycles_ = 0;
public:
    explicit AudioRefillPass(uint32_t engine) {
        auto& trace = audioResamplerTrace;
        if (!trace.refillWindow()) return;
        trace.begin(call_, true);
        if (!call_.record) return;
        call_.record->descriptor = engine;
        call_.record->caller = 0x827D9188;
        QueryThreadCycleTime(GetCurrentThread(), &cycles_);
        activeAudioRefill = &stats_;
    }
    ~AudioRefillPass() {
        if (!call_.record) return;
        stats_.end = audioRefillClock();
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(GetCurrentThread(), &cycles) && cycles >= cycles_)
            stats_.threadCycles = cycles - cycles_;
        call_.record->result = uint32_t(stats_.streams);
        std::memcpy(call_.record->before, &stats_, 88);
        std::memcpy(call_.record->after, reinterpret_cast<const uint8_t*>(&stats_) + 88, 88);
        activeAudioRefill = previous_;
    }
};
}
