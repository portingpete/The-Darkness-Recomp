#pragma once
#include <cstdint>
#include <bit>
#include <algorithm>

namespace DarkRecomp {

#if defined(_MSC_VER)
    #define DARK_BYTESWAP16(x) _byteswap_ushort(x)
    #define DARK_BYTESWAP32(x) _byteswap_ulong(x)
    #define DARK_BYTESWAP64(x) _byteswap_uint64(x)
#else
    #define DARK_BYTESWAP16(x) __builtin_bswap16(x)
    #define DARK_BYTESWAP32(x) __builtin_bswap32(x)
    #define DARK_BYTESWAP64(x) __builtin_bswap64(x)
#endif

// Big-Endian wrapper type matching Xenon guest memory ordering
template<typename T>
struct be {
    T raw;

    constexpr be() : raw(0) {}
    constexpr be(T val) { set(val); }

    constexpr operator T() const { return get(); }

    constexpr be& operator=(T val) {
        set(val);
        return *this;
    }

private:
    constexpr T get() const {
        if constexpr (sizeof(T) == 1) {
            return raw;
        } else if constexpr (sizeof(T) == 2) {
            uint16_t v = std::bit_cast<uint16_t>(raw);
            v = DARK_BYTESWAP16(v);
            return std::bit_cast<T>(v);
        } else if constexpr (sizeof(T) == 4) {
            uint32_t v = std::bit_cast<uint32_t>(raw);
            v = DARK_BYTESWAP32(v);
            return std::bit_cast<T>(v);
        } else if constexpr (sizeof(T) == 8) {
            uint64_t v = std::bit_cast<uint64_t>(raw);
            v = DARK_BYTESWAP64(v);
            return std::bit_cast<T>(v);
        }
        return raw;
    }

    constexpr void set(T val) {
        if constexpr (sizeof(T) == 1) {
            raw = val;
        } else if constexpr (sizeof(T) == 2) {
            uint16_t v = std::bit_cast<uint16_t>(val);
            raw = std::bit_cast<T>(DARK_BYTESWAP16(v));
        } else if constexpr (sizeof(T) == 4) {
            uint32_t v = std::bit_cast<uint32_t>(val);
            raw = std::bit_cast<T>(DARK_BYTESWAP32(v));
        } else if constexpr (sizeof(T) == 8) {
            uint64_t v = std::bit_cast<uint64_t>(val);
            raw = std::bit_cast<T>(DARK_BYTESWAP64(v));
        } else {
            raw = val;
        }
    }
};

union Register64 {
    uint64_t u64;
    int64_t  s64;
    uint32_t u32;
    int32_t  s32;
    uint16_t u16[4];
    int16_t  s16[4];
    uint8_t  u8[8];
    int8_t   s8[8];
    double   f64;
    float    f32[2];
};

union Vector128 {
    uint8_t  u8[16];
    int8_t   s8[16];
    uint16_t u16[8];
    int16_t  s16[8];
    uint32_t u32[4];
    int32_t  s32[4];
    uint64_t u64[2];
    int64_t  s64[2];
    float    f32[4];
};

} // namespace DarkRecomp

#include "ppc_context.h"

namespace DarkRecomp {
    using PPCContext = ::PPCContext;
}
