#pragma once
#include <cstdint>

namespace DarkRecomp::Native {
// The host frequency is fixed for the process and must be positive. Keep the
// original unsigned quotient/remainder result, including its final wrap.
template<uint64_t GuestFrequency> class TimebaseScale {
    static_assert(GuestFrequency > 0 && GuestFrequency <= UINT32_MAX);
    uint64_t frequency_;
    uint64_t integralMultiplier_;
public:
    explicit constexpr TimebaseScale(uint64_t frequency)
        : frequency_(frequency), integralMultiplier_(frequency && GuestFrequency % frequency == 0
              ? GuestFrequency / frequency : 0) {}
    constexpr uint64_t operator()(uint64_t ticks) const {
        if constexpr (GuestFrequency == 49875000) {
            // Reduce 49,875,000/10,000,000 to 399/80. Decompose before
            // multiplication so ticks*399 cannot overflow before division.
            if (frequency_ == 10000000)
                return ticks / 80 * 399 + ticks % 80 * 399 / 80;
        }
        if (integralMultiplier_) return ticks * integralMultiplier_;
        return ticks / frequency_ * GuestFrequency +
            ticks % frequency_ * GuestFrequency / frequency_;
    }
};
}
