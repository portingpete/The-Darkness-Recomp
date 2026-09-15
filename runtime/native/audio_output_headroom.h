#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace DarkRecomp::Native {

// XAudio2 stores one row per destination channel. Summing several individually
// limited guest channels can still overload that destination (5.1 -> stereo
// sums L + 0.708*C + 0.708*BL). Use one constant gain for the entire source to
// preserve the default speaker mapping, stereo balance and sound envelopes.
inline float audioOutputHeadroom(std::span<const float> matrix, unsigned sourceChannels) {
    if (!sourceChannels || matrix.empty() || matrix.size() % sourceChannels) return 0;
    double largest = 1;
    for (size_t row = 0; row < matrix.size(); row += sourceChannels) {
        double sum = 0;
        for (unsigned source = 0; source < sourceChannels; ++source) {
            const float coefficient = matrix[row + source];
            if (!std::isfinite(coefficient)) return 0;
            sum += std::abs(double(coefficient));
        }
        largest = (std::max)(largest, sum);
    }
    // No attenuation for one-to-one surround output or already bounded maps.
    return largest == 1 ? 1.0f : std::nextafter(float(1.0 / largest), 0.0f);
}

} // namespace DarkRecomp::Native
