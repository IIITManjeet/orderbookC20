#pragma once

#include <cstddef>
#include <new>

namespace ob {

#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define OB_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define OB_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define OB_PREFETCH(p) __builtin_prefetch(p)
    #define OB_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
    #define OB_LIKELY(x)   (x)
    #define OB_UNLIKELY(x) (x)
    #define OB_PREFETCH(p) ((void)0)
    #define OB_ALWAYS_INLINE inline
#endif

}  // namespace ob
