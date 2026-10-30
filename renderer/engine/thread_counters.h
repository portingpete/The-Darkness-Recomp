#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace DarkRecomp::Native {
// Monotonic telemetry only: draw/resource publication has its own locks and
// ownership. Keep a Writer on its acquiring thread. Process-lifetime lanes
// preserve totals after that thread exits; overflow writers share an RMW lane.
template<size_t Count,size_t PrivateWriters=64>
class ThreadCounters {
    struct alignas(64) Lane {std::array<std::atomic<uint64_t>,Count> values{};};
    std::array<Lane,PrivateWriters+1> lanes_;
    std::atomic<size_t> next_{};
public:
    class Writer {
        friend class ThreadCounters;
        Lane* lane_;
        bool shared_;
        Writer(Lane* lane,bool shared):lane_(lane),shared_(shared) {}
    public:
        void increment(size_t counter) const noexcept {
            auto& value=lane_->values[counter];
            if(shared_)value.fetch_add(1,std::memory_order_relaxed);
            else value.store(value.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        }
    };
    Writer acquireWriter() noexcept {
        auto next=next_.load(std::memory_order_relaxed);
        while(next<PrivateWriters) {
            if(next_.compare_exchange_weak(next,next+1,std::memory_order_relaxed))
                return Writer(&lanes_[next],false);
        }
        return Writer(&lanes_[PrivateWriters],true);
    }
    std::array<uint64_t,Count> snapshot() const noexcept {
        std::array<uint64_t,Count> sums{};
        for(const auto& lane:lanes_)for(size_t i=0;i<Count;++i)
            sums[i]+=lane.values[i].load(std::memory_order_relaxed);
        return sums;
    }
};
}
