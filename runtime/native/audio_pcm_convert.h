#pragma once
#include "audio_driver.h"
#include <cstring>
#ifdef DARK_NATIVE_SSSE3
#include <tmmintrin.h>
#endif

namespace DarkRecomp::Native {
#ifdef DARK_NATIVE_SSSE3
#ifdef __clang__
[[gnu::target("ssse3")]]
#endif
inline __m128 audioLoadPlanarBEFloat4(const uint8_t* source, __m128i endian) {
    return _mm_castsi128_ps(_mm_shuffle_epi8(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(source)), endian));
}
#endif
#if defined(DARK_NATIVE_SSSE3) && defined(__clang__)
[[gnu::target("ssse3")]]
#endif
inline void audioConvertPlanarBEFloatSamples(const uint8_t* planarBE, float* interleavedOut) {
#ifdef DARK_NATIVE_SSSE3
    static_assert(kAudioChannels == 6 && kAudioFramesPerSubmit % 4 == 0);
    const __m128i endian = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; frame += 4) {
        const uint8_t* source = planarBE + size_t(frame) * 4;
        constexpr size_t stride = size_t(kAudioFramesPerSubmit) * 4;
        __m128 a = audioLoadPlanarBEFloat4(source, endian);
        __m128 b = audioLoadPlanarBEFloat4(source + stride, endian);
        __m128 c = audioLoadPlanarBEFloat4(source + stride * 2, endian);
        __m128 d = audioLoadPlanarBEFloat4(source + stride * 3, endian);
        const __m128 e = audioLoadPlanarBEFloat4(source + stride * 4, endian);
        const __m128 f = audioLoadPlanarBEFloat4(source + stride * 5, endian);
        _MM_TRANSPOSE4_PS(a, b, c, d);
        const __m128 ef01 = _mm_unpacklo_ps(e, f), ef23 = _mm_unpackhi_ps(e, f);
        float* out = interleavedOut + size_t(frame) * kAudioChannels;
        // Six stores cover four six-channel frames. All operations move bits;
        // no arithmetic quiets NaNs or changes signed zero/subnormal samples.
        _mm_storeu_ps(out, a);
        _mm_storeu_ps(out + 4, _mm_shuffle_ps(ef01, b, _MM_SHUFFLE(1, 0, 1, 0)));
        _mm_storeu_ps(out + 8, _mm_shuffle_ps(b, ef01, _MM_SHUFFLE(3, 2, 3, 2)));
        _mm_storeu_ps(out + 12, c);
        _mm_storeu_ps(out + 16, _mm_shuffle_ps(ef23, d, _MM_SHUFFLE(1, 0, 1, 0)));
        _mm_storeu_ps(out + 20, _mm_shuffle_ps(d, ef23, _MM_SHUFFLE(3, 2, 3, 2)));
    }
#else
    for (uint32_t frame = 0; frame < kAudioFramesPerSubmit; ++frame) {
        for (uint32_t ch = 0; ch < kAudioChannels; ++ch) {
            const uint8_t* src = planarBE + (size_t(ch) * kAudioFramesPerSubmit + frame) * 4;
            const uint32_t bits = (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) |
                                  (uint32_t(src[2]) << 8) | uint32_t(src[3]);
            std::memcpy(interleavedOut + size_t(frame) * kAudioChannels + ch, &bits, 4);
        }
    }
#endif
}
}
